#include "mcts.h"

#include <torch/torch.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <unordered_map>

#include "config.h"
#include "env.h"
#include "hash.h"
#include "policy.h"
#include "train_batch.h"
#include "util.h"

typedef std::tuple<std::vector<std::vector<int>>, std::vector<int>, std::vector<int>> State;

std::unordered_map<hash_t, std::pair<State, NodeInfo>> RewardMeanStdMemo;
std::unordered_map<hash_t, std::pair<State, GNNInfo>> PolicyValueMemo;

void Node::init() {
    // after graph, adj_black, adj_white are set
    num_nodes = graph.num_nodes;
    if (is_end(graph)) return;
    children.resize(num_nodes * 2);
    visit_cnt.resize(num_nodes * 2);
    visit_cnt_sum = 0;
    adj_hash_labeled = get_adj_hash_labeled(graph, adj_black, adj_white);
    std::tie(reward_mean, reward_std) = get_mean_std();
    std::tie(policy, value) = get_gnn_estimate();
    assert((int)policy.size() == num_nodes * 2);
    assert((int)value.size() == num_nodes * 2);
}

Node::Node() {}
Node::Node(const Graph& graph)
    : parent(nullptr),
      last_action(-1),
      graph(graph),
      adj_black(graph.num_nodes),
      adj_white(graph.num_nodes),
      last_reward(0) {
    init();
}
Node::Node(const Graph& graph, const std::vector<int>& adj_black, const std::vector<int>& adj_white)
    : parent(nullptr), last_action(-1), graph(graph), adj_black(adj_black), adj_white(adj_white), last_reward(0) {
    init();
}
Node::Node(std::shared_ptr<Node> parent, int last_action)
    : parent(parent), last_action(last_action), adj_black(parent->adj_black), adj_white(parent->adj_white) {
    graph = step(parent->graph, last_action, adj_black, adj_white, last_reward);
    init();
}

GNNInfo Node::get_gnn_estimate() {
    if (PolicyValueMemo.count(adj_hash_labeled)) {
        auto& p = PolicyValueMemo[adj_hash_labeled];
        if (std::make_tuple(graph.adj_list, adj_black, adj_white) == p.first) return p.second;
    }
    torch::Tensor policy_tensor, value_tensor;
    std::tie(policy_tensor, value_tensor) = global_policy->infer_one(graph, adj_black, adj_white);
    std::vector<float> policy_pred = tensor_to_vector<float>(policy_tensor);
    std::vector<float> value_pred = tensor_to_vector<float>(value_tensor);
    PolicyValueMemo[adj_hash_labeled] =
        std::make_pair(std::make_tuple(graph.adj_list, adj_black, adj_white), make_pair(policy_pred, value_pred));
    return PolicyValueMemo[adj_hash_labeled].second;
}

// http://tadaoyamaoka.hatenablog.com/entry/2017/12/10/002854
std::vector<float> random_dirichlet(int n, double alpha) {
    std::gamma_distribution<float> gamma(alpha, 1.0);
    std::vector<float> ret(n);
    for (int i = 0; i < n; i++) ret[i] = gamma(rnd);
    float sum = std::accumulate(ret.begin(), ret.end(), 0.0);
    for (int i = 0; i < n; i++) ret[i] /= sum;
    return ret;
}

int Node::best_child(bool add_noise) {
    std::vector<float> ucb(num_nodes * 2);
    std::vector<float> fixed_policy = policy;
    if (add_noise) {
        std::vector<float> noise = random_dirichlet(num_nodes * 2, cfg::dirichlet_alpha);
        for (int i = 0; i < num_nodes * 2; i++) {
            fixed_policy[i] = fixed_policy[i] * (1 - cfg::dirichlet_eps) + noise[i] * cfg::dirichlet_eps;
        }
    }
    for (int i = 0; i < num_nodes * 2; i++) {
        ucb[i] = value[i] + cfg::alpha * std::sqrt(visit_cnt_sum) * fixed_policy[i] / (1 + visit_cnt[i]);
    }
    return randomized_argmax(ucb);
}

float normalize(float x, float m, float s) {
    if (cfg::use_sigmoid) {
        return std::tanh(((x - m) / (s + cfg::eps)) / cfg::beta);
    } else {
        return (x - m) / (s + cfg::eps);
    }
}

float unnormalize(float v, float m, float s) {
    if (cfg::use_sigmoid) {
        assert(1 - v + cfg::eps > 0);
        return std::log((1 + v) / (1 - v + cfg::eps)) / 2 * cfg::beta;
    } else {
        return m + v * s;
    }
}

float Node::state_value() {
    if (is_end(graph)) return 0;
    return unnormalize(max(value), reward_mean, reward_std);
}

std::vector<float> Node::pi(float tau) {
    std::vector<float> prob(num_nodes * 2);
    int max_visit_cnt = *std::max_element(visit_cnt.begin(), visit_cnt.end());
    if (tau == 0) {
        float sum = 0;
        for (int i = 0; i < num_nodes * 2; i++) {
            if (visit_cnt[i] == max_visit_cnt) {
                prob[i] = 1;
                sum += 1;
            }
        }
        for (int i = 0; i < num_nodes * 2; i++) prob[i] /= sum;
        return prob;
    }
    for (int i = 0; i < num_nodes * 2; i++) prob[i] = std::pow(visit_cnt[i], 1.0 / tau);
    float sum = std::accumulate(prob.begin(), prob.end(), 0.0);
    for (int i = 0; i < num_nodes * 2; i++) prob[i] /= sum;
    return prob;
}

int random_play(const Graph& g, const std::vector<int>& adj_black, const std::vector<int>& adj_white) {
    int n = g.num_nodes;
    std::vector<int> color(n);
    for (int i = 0; i < n; i++) color[i] = rnd() & 1;
    int ret = 0;
    // removed & remain
    for (int i = 0; i < n; i++) {
        ret += (color[i] ? adj_black : adj_white)[i];
    }
    // remain & remain
    for (auto& p : g.edge_list) {
        int u = p.first, v = p.second;
        if (color[u] != color[v]) ret++;
    }
    return ret;
}

NodeInfo Node::get_mean_std() {
    if (RewardMeanStdMemo.count(adj_hash_labeled)) {
        auto& p = RewardMeanStdMemo[adj_hash_labeled];
        if (std::make_tuple(graph.adj_list, adj_black, adj_white) == p.first) return p.second;
    }
    std::vector<int> reward(cfg::num_play);
    for (int i = 0; i < cfg::num_play; i++) reward[i] = random_play(graph, adj_black, adj_white);
    RewardMeanStdMemo[adj_hash_labeled] =
        std::make_pair(std::make_tuple(graph.adj_list, adj_black, adj_white), mean_std(reward));
    return RewardMeanStdMemo[adj_hash_labeled].second;
}

void update_value(std::shared_ptr<Node> node, float v, int idx) {
    // update mean
    node->value[idx] = (node->value[idx] * node->visit_cnt[idx] + v) / (node->visit_cnt[idx] + 1);
}

void update_parent(std::shared_ptr<Node> node, float v) {
    std::shared_ptr<Node> parent = node->parent;
    assert(parent);
    float normalized_value = normalize(v, parent->reward_mean, parent->reward_std);
    if (parent->visit_cnt[node->last_action] == 0) {
        parent->value[node->last_action] = normalized_value;
    } else {
        update_value(parent, normalized_value, node->last_action);
    }
    parent->visit_cnt[node->last_action]++;
    parent->visit_cnt_sum++;
}

float rollout(std::shared_ptr<Node> root, bool add_noise) {
    std::shared_ptr<Node> node = root;
    bool first = true;
    while (!is_end(node->graph)) {
        int act = node->best_child(first && add_noise);
        first = false;
        if (node->children[act]) {
            node = node->children[act];
        } else {
            node->children[act] = std::make_shared<Node>(node, act);
            break;
        }
    }
    float v = node->state_value();
    while (node != root) {
        v += node->last_reward;
        update_parent(node, v);
        node = node->parent;
    }
    return v;
}

std::vector<float> get_improved_pi(std::shared_ptr<Node> root, float tau, bool add_noise = false) {
    assert(root->num_nodes);
    while (root->visit_cnt_sum < std::ceil(cfg::rollout_coef * root->num_nodes)) rollout(root, add_noise);
    return root->pi(tau);
}

float train() {
    assert(TrainData.n);
    int size = TrainData.n;
    float loss = 0;
    std::vector<int> perm(size);
    for (int i = 0; i < size; i++) perm[i] = i;
    std::shuffle(perm.begin(), perm.end(), rnd);
    for (int i = 0; perm.size() % cfg::batch_size; i++) perm.push_back(perm[i]);
    for (int i = 0; i < (int)perm.size(); i += cfg::batch_size) {
        std::vector<int> idxes(cfg::batch_size);
        for (int j = 0; j < cfg::batch_size; j++) idxes[j] = perm[i + j];
        loss += global_policy->train(idxes);
    }
    return loss;
}

int test() {
    Graph graph = CurrentTestGraph;
    int ret = 0;
    std::vector<int> adj_black(graph.num_nodes), adj_white(graph.num_nodes);
    while (!is_end(graph)) {
        auto policy_value = global_policy->infer_one(graph, adj_black, adj_white);
        int action = randomized_argmax(tensor_to_vector<float>(policy_value.first));
        int reward;
        graph = step(graph, action, adj_black, adj_white, reward);
        ret += reward;
    }
    return ret;
}

int test_by_mcts() {
    return 0;
}

void generate_train_data(const std::string& filename) {
    std::string path = cfg::save_dir + "data/" + filename;
    Graph graph = CurrentGraph;
    std::vector<Graph> graphs;
    std::vector<std::vector<int>> adj_blacks, adj_whites;
    std::vector<int> actions;
    std::vector<std::vector<float>> pis;
    std::vector<float> means, stds;
    std::vector<float> rewards;
    int step_cnt = 0;
    std::shared_ptr<Node> root = std::make_shared<Node>(graph);
    std::vector<int> adj_black(graph.num_nodes), adj_white(graph.num_nodes);
    while (!is_end(graph)) {
        graphs.push_back(graph);
        adj_blacks.push_back(adj_black);
        adj_whites.push_back(adj_white);
        means.push_back(root->reward_mean);
        stds.push_back(root->reward_std);
        float tau = step_cnt < cfg::initial_phase_len ? 1 : 0;
        std::vector<float> pi = get_improved_pi(root, tau, true);
        int action = weighted_choose(pi);
        pis.push_back(pi);
        root = root->children[action];
        int reward;
        graph = step(graph, action, adj_black, adj_white, reward);
        actions.push_back(action);
        rewards.push_back(reward);
        assert(graph.adj_list == root->graph.adj_list);
        step_cnt++;
    }
    int size = rewards.size();
    for (int i = size - 2; i >= 0; i--) rewards[i] += rewards[i + 1];
    for (int i = 0; i < size; i++) rewards[i] = normalize(rewards[i], means[i], stds[i]);
    TrainBatch batch(graphs, adj_blacks, adj_whites, actions, pis, rewards);
    save_train_data(filename, batch);
}
