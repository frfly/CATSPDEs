﻿#include <memory>
// finite elements to use
#include "Triangle_P0_Lagrange.hpp"
#include "Triangle_P1_Lagrange.hpp"
#include "Triangle_P2_Lagrange.hpp"
#include "Triangle_P1_CrouzeixRaviart.hpp"
// interpolant for diffusion
#include "FEInterpolant.hpp"
// assembler
#include "DivGradFEM.hpp"
// solvers
#include "ProjectionSolvers.hpp"
#include "NonlinearSolvers.hpp"
// preconditioner
#include "Multigrid.hpp"
// pi
#include "constants.hpp"
// for small rotation matrix
#include "DenseMatrix.hpp"
// for 1D interpolant of mu_bar_iron
#include "Segment_P1_Lagrange.hpp"

using std::vector;
using std::string;
using boost::get;
using namespace FEM::DivGrad;
using namespace ProjectionSolvers;
using namespace NonlinearSolvers;

// shuffle initial guess 
// in order to introduce all possible freq in the error, thus test MG
#include <random>
auto shuffle(Index n) {
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<> dis(-1., 1.);
	vector<double> x(n);
	for (auto& el : x) el = dis(gen);
	return x;
}

int main() {
	string iPath("Mathematica/preprocessing/"), oPath("Mathematica/postprocessing/");
	auto& logger = SingletonLogger::instance();
	try {
		logger.beg("set up mesh");
			vector<string> meshType { "mesh_good.nt", "mesh_coarse.nt", "mesh_nonconform.nt" };
			auto meshTypeIndex = logger.opt("mesh type", meshType);
			Triangulation Omega;
			Omega.import(iPath + meshType[meshTypeIndex]);
			Omega.computeNeighbors().enumerateRibs();	
			// special subdomains of Omega
			Quadrilateral2D 
				// parts of magnet
				magnetLeft {
					{ { 0., 0. }, { .02, 0. }, { .02, .06 }, { 0., .06 } }
				},
				magnetMiddle {
					{ { .04, .04 }, { .04, .002 }, { .06, .002 }, { .06, .04 } }
				},
				magnetRight {
					{ { .08, .06 }, { .08, 0. }, { .1, 0. }, { .1, .06 } }
				},
				magnetTop {
					{ { 0.1, 0.06 }, { 0, 0.06 }, { 0, 0.04 }, { 0.1, 0.04 } }
				},
				// wires
				left { // left wire
					{ { .025, .025 }, { .035, .025 }, { .035, .035 }, { .025, .035 } }
				},
				right { // right ″
					{ { .065, 0.025 }, { .075, .025 }, { .075, .035 }, { .065, .035 } }
				};
			Predicate2D magnet = [&](Node2D const & p) {
				return nodeInElement(magnetLeft, p)
				    || nodeInElement(magnetMiddle, p)
				    || nodeInElement(magnetRight, p)
				    || nodeInElement(magnetTop, p);
			};
		logger.end();
		logger.beg("set up PDE and BCs");
			// current density
			double J_z = 1e+06;
			logger.inp("set current density J_z", J_z);
			// iron permeability
			double mu_roof_iron = 1000.;
			logger.inp("set iron permeability mu_roof_iron", mu_roof_iron);
			// vacuum permeability
			
			// right value:
			// double const mu_0 = 4. * PI * 1e-7; 
			// for testing:
			double const mu_0 = 1.;
			
			ScalarField2D 
				diffusion = [&](Node2D const & p) { // linear case
					double mu_roof = magnet(p) ? mu_roof_iron : 1.;
					return 1. / mu_roof;
				},
				force = [&](Node2D const & p) {
					if (nodeInElement(left, p)) return mu_0 * J_z;
					if (nodeInElement(right, p)) return - mu_0 * J_z;
					return 0.;
				},
				reaction = [](Node2D const) { return 0.; },
			    NeumannValue = reaction, RobinCoefficient = reaction, DirichletCondition = reaction;
				Predicate2D naturalBCsPredicate = [](Node2D const p) { return p[1] == 0.; };
			// PDE
			DiffusionReactionEqn2D PoissonEqn { diffusion, reaction, force };
			// BCs
			ScalarBoundaryCondition2D 
				RobinBC {
					{ RobinCoefficient, NeumannValue }, // n . (a ∇u) + R u = N
					naturalBCsPredicate
				}, 
				DirichletBC { DirichletCondition };
		logger.end();
		logger.beg("build and export interpolants for diffusion and force");
			TriangularScalarFEInterpolant 
				diffisionInterp { diffusion, Triangle_P0_Lagrange::instance(), Omega },
				forceInterp { force, Triangle_P0_Lagrange::instance(), Omega };
			export(diffisionInterp.DOFs(), oPath + "diffusion.dat");
			export(forceInterp.DOFs(), oPath + "force.dat");
			Omega.export(oPath + "mesh_0.ntnr", { { "format", "NTNR" } });
		logger.end();
		//logger.beg("refine mesh");
		//	Omega.refine(numbOfMeshLevels);
		//logger.end();
		
		
		logger.beg("set solver data");
			// method of solving SLAE
			auto method = logger.opt("choose solving technique", {
				"MG",
				"P_MG CG",
				"P_ILU(0) CG",
				"CG"
			});
			// FE
			vector<TriangularScalarFiniteElement*> FEs {
				&Triangle_P1_Lagrange::instance(),
				&Triangle_P2_Lagrange::instance(),
				&Triangle_P1_CrouzeixRaviart::instance()
			};
			auto FEIndex = logger.opt("choose finite element", { "Lagrange P1", "Lagrange P2", "Crouzeix-Raviart P1" });
			auto& FE = *FEs[FEIndex];
			// numb of refinements
			Index numbOfMeshLevels;
			logger.inp("numb of mesh levels (refinements)", numbOfMeshLevels);
			// iterations logger
			Index i_log;
			logger.inp("log every nth iteration of the solver, n", i_log);
			// max numb of iters
			Index maxNumbOfIterations;
			logger.inp("max numb of iterations", maxNumbOfIterations);
			// tolerance
			double eps;
			logger.inp("set eps for solver", eps);
			// stopping criterion
			auto stop = (StoppingCriterion)logger.opt("stopping criterion", { "absolute", "relative" });
			logger.beg("set MG data");
				logger.wrn("these values make sense for (MG) and (P_MG СG) solvers only");
				// smoothing
				Index nu;
				logger.inp("numb of pre- and post-smoothing iterations", nu);
				// cycle type
				auto gamma = logger.opt("set recursive calls type", { "V-cycle", "W-cycle" });
				++gamma;
				// iters for precond
				Index numbOfInnerIterations;
				logger.inp("numb of iterations for inner solver", numbOfInnerIterations);
			logger.end();
		logger.end();
		logger.beg("run method");
			vector<double> x_linear;
			if      (method == 0) /* MG */ {
				logger.beg("set up mg");
					Multigrid<SymmetricCSlCMatrix<double>> MG {
							FE, Omega, numbOfMeshLevels,
							[&](Triangulation const & Omega) {
								return assembleSystem(PoissonEqn, Omega, RobinBC, DirichletBC, FE);
							},
							TransferType::canonical
					};
					auto smoother = [&](SymmetricCSlCMatrix<double>& A, vector<double> const & b, vector<double> const & x_0) {
						return Smoothers::SSOR(A, b, x_0, 1., nu, 0., StoppingCriterion::absolute, 0);
					};
				logger.end();
				logger.beg("solve w/ MG");
					auto& A = MG.A();
					auto& b = MG.b();
					x_linear = shuffle(A.getOrder());
					// initial residual
					double r_0 = norm(b - A * x_linear), r = r_0, r_new;
					logInitialResidual(r_0, maxNumbOfIterations);
					// MG as a stand-alone iteration
					Index i;
					for (i = 1; i <= maxNumbOfIterations; ++i) {
						logger.mute = true;
						x_linear = MG(numbOfMeshLevels, b, x_linear, smoother, gamma);
						logger.mute = false;
						r_new = norm(b - A * x_linear);
						if (i_log && i % i_log == 0) logResidualReduction(r, r_new, i, maxNumbOfIterations);
						r = r_new;
						if (stop == StoppingCriterion::absolute && r < eps) break;
						else if (stop == StoppingCriterion::relative && r / r_0 < eps) break;
					}
					logFinalResidual(r_0, r_new, i > maxNumbOfIterations ? maxNumbOfIterations : i, maxNumbOfIterations);
					if (i > maxNumbOfIterations) logger.wrn("MG failed");
				logger.end();
			}
			else if (method == 1) /* P_MG CG */ {
				logger.beg("build preconditioner");
					Multigrid<SymmetricCSlCMatrix<double>> MG {
							FE, Omega, numbOfMeshLevels,
							[&](Triangulation const & Omega) {
								return assembleSystem(PoissonEqn, Omega, RobinBC, DirichletBC, FE);
							},
							TransferType::canonical
					};
					auto smoother = [&](SymmetricCSlCMatrix<double>& A, vector<double> const & b, vector<double> const & x_0) {
						return Smoothers::SSOR(A, b, x_0, 1., nu, 0., StoppingCriterion::absolute, 0);
					};
					Index n = MG.A().getOrder();
					Preconditioner P = [&](vector<double> const & x) {
						vector<double> y(n);
						auto cond = logger.mute;
						logger.mute = true;
						for (Index i = 0; i < numbOfInnerIterations; ++i)
							y = MG(numbOfMeshLevels, x, y, smoother, gamma);
						logger.mute = cond;
						return y;
					};
				logger.end();
				logger.beg("solve linear problem");
					auto& A = MG.A();
					auto& b = MG.b();
					x_linear = Krylov::PCG(P, A, b, shuffle(A.getOrder()), maxNumbOfIterations, eps, stop, i_log);
				logger.end();
			}
			else if (method == 2) /* P_ILU(0) CG */ {
				logger.beg("refine mesh");
					Omega.refine(numbOfMeshLevels);
				logger.end();
				logger.beg("assemble system");
					auto system = assembleSystem(PoissonEqn, Omega, RobinBC, DirichletBC, FE);
					auto& A = get<0>(system);
					auto& b = get<1>(system);
				logger.end();
				logger.beg("build ILU(0)");
					auto LDLT = A;
					LDLT.decompose();
					auto P = [&](vector<double> const & x) {
						return LDLT.backSubst(LDLT.diagSubst(LDLT.forwSubst(x, 0.)), 0.);
					};
				logger.end();
				logger.beg("solve linear problem");
					x_linear = Krylov::PCG(P, A, b, shuffle(A.getOrder()), maxNumbOfIterations, eps, stop, i_log);
				logger.end();
			}
			else if (method == 3) /* CG */ {
				logger.beg("refine mesh");
					Omega.refine(numbOfMeshLevels);
				logger.end();
				logger.beg("assemble system");
					auto system = assembleSystem(PoissonEqn, Omega, RobinBC, DirichletBC, FE);
					auto& A = get<0>(system);
					auto& b = get<1>(system);
				logger.end();
				logger.beg("solve linear problem");
					x_linear = Krylov::CG(A, b, shuffle(A.getOrder()), maxNumbOfIterations, eps, stop, i_log);
				logger.end();
			}
		logger.end();
		logger.beg("compute interpolant for magnetic field");
			TriangularScalarFEInterpolant x_linear_interp { x_linear, FE, Omega };
			Index activeElementIndex;
			DenseMatrix<double> rotate {
				{  0., 1. },
				{ -1., 0. }
			};
			auto B_linear = [&](Node2D const & p) -> Node2D {
				return rotate * x_linear_interp.grad(p, activeElementIndex);
			};
			auto Bx_linear = [&](Node2D const & p) { return B_linear(p)[0]; };
			auto By_linear = [&](Node2D const & p) { return B_linear(p)[1]; };
			auto Bn_linear = [&](Node2D const & p) { return norm(B_linear(p)); };
			TriangularScalarFEInterpolant 
				Bx_linear_interp { Bx_linear, FE/*Triangle_P0_Lagrange::instance()*/, Omega, activeElementIndex },
				By_linear_interp { By_linear, FE/*Triangle_P0_Lagrange::instance()*/, Omega, activeElementIndex },
				Bn_linear_interp { Bn_linear, FE/*Triangle_P0_Lagrange::instance()*/, Omega, activeElementIndex };
		logger.end();
		logger.beg("export soln vector");
			export(x_linear, oPath + "x_linear.dat");
			export(Bx_linear_interp.DOFs(), oPath + "Bx_linear.dat");
			export(By_linear_interp.DOFs(), oPath + "By_linear.dat");
			export(Bn_linear_interp.DOFs(), oPath + "Bn_linear.dat");
		logger.end();
		/* */
		if (logger.yes("solve non-linear problem")) {
			//logger.beg("set non-linear solver data");
			//	Index i_log_picard;
			//	logger.inp("log every nth Picard iteration, n", i_log_picard);
			//	double omega;
			//	logger.inp("relaxation for Picard", omega);
			//logger.end();
			//logger.beg("build interpolant for mu_roof_iron");
			//	SegmentMesh BnValues;
			//	BnValues.import(iPath + "Bn_values.dat");
			//	double Bn_min = BnValues.getNodes().front(), Bn_max = BnValues.getNodes().back();
			//	vector<double> muValues(BnValues.numbOfNodes());
			//	import(muValues, iPath + "mu_values.dat");
			//	SegmentFEInterpolant mu_roof_iron_interp { muValues, Segment_P1_Lagrange::instance(), BnValues };
			//	// relative permeability of iron as a function of ||B||
			//	auto mu_roof_iron = [&](double Bn) {
			//		// (1) extrapolation
			//		if (Bn < Bn_min) 
			//			return muValues.front();
			//		if (Bn > Bn_max) 
			//			return Bn * muValues.back() / (Bn_max + Bn * muValues.back() - Bn_max * muValues.back());
			//		// (2) healthy case
			//		return mu_roof_iron_interp(Bn);
			//	};
			//logger.end();
			//logger.beg("build preconditioner and initial guess");
			//	Preconditioner N;
			//	std::shared_ptr<decltype(MG)> MGN;
			//	vector<double> x_initial;
			//	if (true/*J_z <= 10e+5*/) { // linear case is a good approximation
			//		logger.log("linear initial approximation");
			//		N = P;
			//		x_initial = x_linear;
			//	}
			//	else if (J_z <= 10e+7) {
			//		Index activeElementIndex;
			//		TriangularScalarFEInterpolant x_linear_interp11 { x_linear, FE, Omega };
			//		PoissonEqn.diffusionTerm() = [&](Node2D const & p) {
			//			double mu_roof = magnet(p)
			//				? mu_roof_iron(norm(x_linear_interp11.grad(p, activeElementIndex)))
			//				: 1.;
			//			return 1. / mu_roof;
			//		};
			//		MGN = std::make_shared<decltype(MG)>(decltype(MG) {
			//			FE, OmegaInitial, numbOfMeshLevels,
			//			[&](Triangulation const & Omega) {
			//				return assembleSystem(PoissonEqn, Omega, RobinBC, DirichletBC, FE);
			//			},
			//			TransferType::canonical
			//		});
			//		N = [&](vector<double> const & x) {
			//			vector<double> y(n);
			//			auto cond = logger.mute;
			//			logger.mute = true;
			//			y = (*MGN)(numbOfMeshLevels, x, y, smoother, 2);
			//			logger.mute = cond;
			//			return y;
			//		};
			//		x_initial = Krylov::PCG(N, MGN->A(), MGN->b(), boost::none, 0, 1e-12, StoppingCriterion::relative, i_log);
			//	}
			//	else {
			//		
			//	}
			//logger.end();
			//logger.beg("solve non-linear problem");
			//	auto phi = [&](vector<double> const & x_old, double& r_norm) {

			//		logger.mute = true;

			//		Index activeElementIndex;
			//		TriangularScalarFEInterpolant x_old_interp { x_old, FE, Omega };
			//		PoissonEqn.diffusionTerm() = [&](Node2D const & p) {
			//			double mu_roof = magnet(p) 
			//				? mu_roof_iron(norm(x_old_interp.grad(p, activeElementIndex)))
			//				: 1.;
			//			return 1. / mu_roof;
			//		};
			//		logger.beg("assemble system");
			//			auto system = assembleSystem(PoissonEqn, Omega, RobinBC, DirichletBC, FE, activeElementIndex);
			//			auto& A = get<0>(system);
			//			auto& b = get<1>(system);
			//		logger.end();
			//		logger.beg("compute new approximation");
			//			//auto x_new = Krylov::PCG(N, A, b, x_old, 50, 10e-12, StoppingCriterion::relative, i_log);
			//			auto x_new = Krylov::CG(A, b, x_old, 0, 1e-12, StoppingCriterion::relative, i_log);
			//		logger.end();

			//		logger.mute = false;

			//		r_norm = norm(b - A * x_old);
			//		return x_new;
			//	};
			//	auto x_nonlinear = PicardIteration(phi, x_initial, omega, 100, 1e-7);
			//logger.end();
			//logger.beg("compute interpolant for magnetic field");
			//	TriangularScalarFEInterpolant x_nonlinear_interp { x_nonlinear, FE, Omega };
			//	auto B_nonlinear = [&](Node2D const & p) -> Node2D {
			//		return rotate * x_nonlinear_interp.grad(p, activeElementIndex);
			//	};
			//	auto Bx_nonlinear = [&](Node2D const & p) { return B_nonlinear(p)[0]; };
			//	auto By_nonlinear = [&](Node2D const & p) { return B_nonlinear(p)[1]; };
			//	auto Bn_nonlinear = [&](Node2D const & p) { return norm(B_nonlinear(p)); };
			//	TriangularScalarFEInterpolant 
			//		Bx_nonlinear_interp { Bx_nonlinear, FE, Omega, activeElementIndex },
			//		By_nonlinear_interp { By_nonlinear, FE, Omega, activeElementIndex },
			//		Bn_nonlinear_interp { Bn_nonlinear, FE, Omega, activeElementIndex };
			//logger.end();
			//logger.beg("export soln vector");
			//	export(x_nonlinear, oPath + "x_nonlinear.dat");
			//	export(Bx_nonlinear_interp.DOFs(), oPath + "Bx_nonlinear.dat");
			//	export(By_nonlinear_interp.DOFs(), oPath + "By_nonlinear.dat");
			//	export(Bn_nonlinear_interp.DOFs(), oPath + "Bn_nonlinear.dat");
			//logger.end();
		}
		/* */
		logger.beg("export mesh");
			Omega.export(oPath + "mesh.ntnr", { { "format", "NTNR" } });
		logger.end();
		logger.exp("stdin.txt");
	}
	catch (std::exception const & e) {
		logger.err(e.what());
	}
	return 0;
}