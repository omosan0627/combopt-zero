#include "env.h"

#include <map>
#include <utility>

bool is_end(const std::vector<int>& adj_black) { 
	for(int v: adj_black) {
		if(!v) return false;
	}
	return true;
}

Graph step(const Graph& g, int action, std::vector<int>& adj_black, std::vector<int>& adj_white, int& reward) {
    reward = 0;
    int n = g.num_nodes;
    assert(n);
    assert((int)adj_black.size() == n);
    assert((int)adj_white.size() == n);
	assert(!adj_black[action]);

	adj_black[action] = 1;

    Graph ret(n);
	std::vector<int> vec(n, 0);
    for (auto& v: g.adj_list[action]) {
		if(!adj_black[v]) vec[v] = 1;
    }
	std::set<std::pair<int, int>> S(g.edge_list.begin(), g.edge_list.end());

	for(int i = 0; i < (int)g.adj_list[action].size(); i++) {
		for(int j = i + 1; j < (int)g.adj_list[action].size(); j++) {
			int u = g.adj_list[action][i];
			int v = g.adj_list[action][j];
			if(u > v) std::swap(u, v);
			if(!adj_black[u] && !adj_black[v] && !S.count(std::make_pair(u, v))) {
				ret.add_edge(u, v);
			}
		}
	}
	for(auto p: g.edge_list) {
		ret.add_edge(p.first, p.second);
	}
    return ret;
}
