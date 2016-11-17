#pragma once
#include "AbstractSparseMatrix.hpp"
#include "AdjacencyList.hpp"

/*
	Alexander �ilyakov, Sep 2016
*/

template <typename T>
class AbstractFEMatrix
	: virtual public AbstractSparseMatrix<T> {
public:
	// construct matrix pattern from adjacency list 
	// of finite element mesh verticies
	virtual AbstractFEMatrix& generatePatternFrom(AdjacencyList const &) = 0;
	// enforce strong BCs
	virtual AbstractFEMatrix& enforceDirichletBCs(std::unordered_map<Index, T> const &, std::vector<T>&) = 0;
};