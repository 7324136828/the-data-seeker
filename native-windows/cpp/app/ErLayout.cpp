#include "ErLayout.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace native_app {
namespace {
constexpr int Margin = 40, LayerGap = 96, CardGap = 48, ComponentGap = 96;
using Index = size_t;
using Order = std::vector<std::vector<Index>>;
using Graph = std::vector<std::vector<Index>>;
int Coordinate(int64_t value) {
    if (value < 0 || value > std::numeric_limits<int>::max()) throw std::overflow_error("The ER diagram exceeds the supported coordinate range.");
    return static_cast<int>(value);
}
struct DisjointSets {
    std::vector<Index> parent, rank;
    explicit DisjointSets(Index size) : parent(size), rank(size) { std::iota(parent.begin(), parent.end(), 0); }
    Index Find(Index node) {
        Index root = node; while (parent[root] != root) root = parent[root];
        while (parent[node] != node) { const auto next = parent[node]; parent[node] = root; node = next; }
        return root;
    }
    void Join(Index a, Index b) {
        a = Find(a); b = Find(b); if (a == b) return;
        if (rank[a] < rank[b]) std::swap(a, b); parent[b] = a; if (rank[a] == rank[b]) ++rank[a];
    }
};
struct Components { std::vector<Index> owner; std::vector<std::vector<Index>> members; };
Components StrongComponents(const Graph& children, const Graph& parents) {
    const Index count = children.size(), unknown = std::numeric_limits<Index>::max();
    std::vector<bool> visited(count); std::vector<Index> finish;
    struct Frame { Index node, next; }; std::vector<Frame> stack;
    for (Index first = 0; first < count; ++first) {
        if (visited[first]) continue;
        visited[first] = true; stack.push_back({first, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next < children[frame.node].size()) {
                const auto next = children[frame.node][frame.next++];
                if (!visited[next]) { visited[next] = true; stack.push_back({next, 0}); }
            } else { finish.push_back(frame.node); stack.pop_back(); }
        }
    }
    Components result; result.owner.assign(count, unknown); std::vector<Index> pending;
    for (auto item = finish.rbegin(); item != finish.rend(); ++item) {
        if (result.owner[*item] != unknown) continue;
        const auto component = result.members.size(); result.members.emplace_back();
        result.owner[*item] = component; pending.push_back(*item);
        while (!pending.empty()) {
            const auto node = pending.back(); pending.pop_back(); result.members.back().push_back(node);
            for (const auto next : parents[node]) if (result.owner[next] == unknown) { result.owner[next] = component; pending.push_back(next); }
        }
        std::sort(result.members.back().begin(), result.members.back().end());
    }
    return result;
}
std::vector<Index> DependencyLayers(const Graph& children, const Components& components) {
    Graph dag(components.members.size()); std::vector<Index> indegree(dag.size()), layer(dag.size());
    for (Index node = 0; node < children.size(); ++node) for (const auto child : children[node])
        if (components.owner[node] != components.owner[child]) dag[components.owner[node]].push_back(components.owner[child]);
    for (auto& next : dag) { std::sort(next.begin(), next.end()); next.erase(std::unique(next.begin(), next.end()), next.end()); for (const auto child : next) ++indegree[child]; }
    using Ready = std::pair<Index, Index>;
    std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
    for (Index group = 0; group < dag.size(); ++group) if (!indegree[group]) ready.push({components.members[group].front(), group});
    while (!ready.empty()) {
        const auto parent = ready.top().second; ready.pop();
        for (const auto child : dag[parent]) {
            layer[child] = std::max(layer[child], layer[parent] + 1);
            if (!--indegree[child]) ready.push({components.members[child].front(), child});
        }
    }
    return layer;
}
struct Score {
    uint64_t crossings = 0, distance = 0;
    bool operator<(const Score& other) const { return crossings < other.crossings || (crossings == other.crossings && distance < other.distance); }
};
struct LocalLayout {
    Index first = 0;
    int width = 0, height = 0;
    std::vector<std::pair<Index, ErLayoutPoint>> positions;
};
LocalLayout ArrangeComponent(const std::vector<Index>& members, const std::vector<ErLayoutNode>& nodes,
    const Graph& children, const Graph& parents, const Components& components, const std::vector<Index>& layers,
    std::vector<int64_t>& centers, std::vector<int>& positionsY, std::vector<Index>& ranks) {
    Index lastLayer = 0; std::set<Index> groups;
    for (const auto node : members) { groups.insert(components.owner[node]); lastLayer = std::max(lastLayer, layers[components.owner[node]]); }
    Order order(lastLayer + 1);
    for (const auto group : groups) order[layers[group]].push_back(group);
    for (auto& layer : order) std::sort(layer.begin(), layer.end(), [&](Index a, Index b) { return components.members[a].front() < components.members[b].front(); });
    std::vector<int64_t> heights(order.size()); std::vector<int> widths(order.size()); std::vector<Index> layerNodes(order.size());
    int64_t maxHeight = 0;
    for (Index layer = 0; layer < order.size(); ++layer) {
        for (const auto group : order[layer]) for (const auto node : components.members[group]) {
            heights[layer] += nodes[node].height + static_cast<int64_t>(CardGap); widths[layer] = std::max(widths[layer], nodes[node].width); ++layerNodes[layer];
        }
        if (layerNodes[layer]) heights[layer] -= CardGap;
        maxHeight = std::max(maxHeight, heights[layer]);
    }
    Coordinate(maxHeight);
    const auto updateLayer = [&](Index layer) {
        int64_t y = (maxHeight - heights[layer]) / 2; Index rank = 0;
        for (const auto group : order[layer]) for (const auto node : components.members[group]) {
            positionsY[node] = Coordinate(y); centers[node] = y * 2 + nodes[node].height; ranks[node] = rank++; y += nodes[node].height + static_cast<int64_t>(CardGap);
        }
    };
    for (Index layer = 0; layer < order.size(); ++layer) updateLayer(layer);
    using EdgePair = std::pair<Index, Index>;
    std::map<EdgePair, std::vector<EdgePair>> byLayers;
    for (const auto parent : members) for (const auto child : children[parent]) {
        const auto from = layers[components.owner[parent]], to = layers[components.owner[child]];
        if (from != to) byLayers[{from, to}].push_back({parent, child});
    }
    const auto score = [&] {
        Score result;
        for (const auto parent : members) for (const auto child : children[parent])
            result.distance += static_cast<uint64_t>(centers[parent] > centers[child] ? centers[parent] - centers[child] : centers[child] - centers[parent]);
        for (const auto& layerPair : byLayers) {
            auto links = layerPair.second;
            std::sort(links.begin(), links.end(), [&](const auto& a, const auto& b) { return ranks[a.first] < ranks[b.first] || (ranks[a.first] == ranks[b.first] && ranks[a.second] < ranks[b.second]); });
            std::vector<uint64_t> fenwick(layerNodes[layerPair.first.second] + 1); uint64_t seen = 0;
            const auto prefix = [&](Index index) { uint64_t total = 0; for (; index; index &= index - 1) total += fenwick[index]; return total; };
            for (Index begin = 0; begin < links.size();) {
                Index end = begin + 1; while (end < links.size() && links[end].first == links[begin].first) ++end;
                for (Index i = begin; i < end; ++i) result.crossings += seen - prefix(ranks[links[i].second] + 1);
                for (Index i = begin; i < end; ++i) for (Index index = ranks[links[i].second] + 1; index < fenwick.size(); index += index & (~index + 1)) ++fenwick[index];
                seen += end - begin; begin = end;
            }
        }
        return result;
    };
    auto bestOrder = order; auto bestScore = score();
    const auto consider = [&] { const auto candidate = score(); if (candidate < bestScore) { bestScore = candidate; bestOrder = order; } };
    const auto sweep = [&](Index layer, bool forward) {
        struct Barycenter { Index group; long double value; }; std::vector<Barycenter> keys;
        for (const auto group : order[layer]) {
            long double sum = 0; Index count = 0;
            for (const auto node : components.members[group]) for (const auto neighbor : (forward ? parents[node] : children[node]))
                if (forward ? layers[components.owner[neighbor]] < layer : layers[components.owner[neighbor]] > layer) { sum += centers[neighbor]; ++count; }
            if (!count) { const auto& groupNodes = components.members[group]; sum = (centers[groupNodes.front()] + centers[groupNodes.back()]) / 2.0L; count = 1; }
            keys.push_back({group, sum / count});
        }
        std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.value < b.value; });
        for (Index i = 0; i < keys.size(); ++i) order[layer][i] = keys[i].group;
        updateLayer(layer);
    };
    // Keep the best sweep, so the crossing objective never worsens from ID order.
    for (int pass = 0; pass < 8; ++pass) {
        for (Index layer = 1; layer < order.size(); ++layer) sweep(layer, true);
        consider();
        for (Index layer = order.size(); layer > 1;) { --layer; sweep(layer - 1, false); }
        consider();
    }
    order = std::move(bestOrder); for (Index layer = 0; layer < order.size(); ++layer) updateLayer(layer);
    LocalLayout result; result.first = members.front(); result.height = Coordinate(maxHeight);
    int64_t x = 0;
    for (Index layer = 0; layer < order.size(); ++layer) {
        for (const auto group : order[layer]) for (const auto node : components.members[group])
            result.positions.push_back({node, {Coordinate(x + (widths[layer] - nodes[node].width) / 2), positionsY[node]}});
        x += widths[layer] + static_cast<int64_t>(LayerGap);
    }
    result.width = Coordinate(x - LayerGap); return result;
}
struct Skyline { int64_t x, y, width; };
std::map<std::string, ErLayoutPoint> Pack(std::vector<LocalLayout> components, const std::vector<ErLayoutNode>& nodes) {
    int64_t widest = 0; long double area = 0;
    for (const auto& component : components) {
        widest = std::max(widest, component.width + static_cast<int64_t>(ComponentGap));
        area += (component.width + static_cast<long double>(ComponentGap)) * (component.height + static_cast<long double>(ComponentGap));
    }
    const auto preferred = static_cast<int64_t>(std::max(900.0L, std::min(1500.0L, std::ceil(std::sqrt(area * 1.4L)))));
    const auto targetWidth = std::max(widest, preferred); Coordinate(targetWidth + Margin);
    std::sort(components.begin(), components.end(), [](const auto& a, const auto& b) {
        const auto areaA = static_cast<int64_t>(a.width) * a.height, areaB = static_cast<int64_t>(b.width) * b.height;
        return areaA > areaB || (areaA == areaB && a.first < b.first);
    });
    std::vector<Skyline> skyline{{0, 0, targetWidth}}; std::map<std::string, ErLayoutPoint> result;
    for (const auto& component : components) {
        const int64_t width = component.width + static_cast<int64_t>(ComponentGap), height = component.height + static_cast<int64_t>(ComponentGap);
        int64_t chosenX = 0, chosenY = std::numeric_limits<int64_t>::max();
        for (Index start = 0; start < skyline.size(); ++start) {
            const auto x = skyline[start].x; if (x + width > targetWidth) continue;
            int64_t y = 0, remaining = width;
            for (Index i = start; remaining > 0 && i < skyline.size(); ++i) { y = std::max(y, skyline[i].y); remaining -= skyline[i].width; }
            if (remaining > 0) continue;
            if (y < chosenY || (y == chosenY && x < chosenX)) { chosenX = x; chosenY = y; }
        }
        if (chosenY == std::numeric_limits<int64_t>::max()) throw std::runtime_error("Cannot place an ER component within the layout canvas.");
        Coordinate(chosenY + height + Margin);
        for (const auto& position : component.positions)
            result[nodes[position.first].id] = {Coordinate(chosenX + position.second.x + Margin), Coordinate(chosenY + position.second.y + Margin)};
        const auto end = chosenX + width; std::vector<Skyline> next;
        for (const auto& strip : skyline) {
            if (strip.x + strip.width <= chosenX || strip.x >= end) next.push_back(strip);
            else {
                if (strip.x < chosenX) next.push_back({strip.x, strip.y, chosenX - strip.x});
                if (strip.x + strip.width > end) next.push_back({end, strip.y, strip.x + strip.width - end});
            }
        }
        next.push_back({chosenX, chosenY + height, width}); std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
        skyline.clear();
        for (const auto& strip : next) {
            if (!skyline.empty() && skyline.back().x + skyline.back().width == strip.x && skyline.back().y == strip.y) skyline.back().width += strip.width;
            else skyline.push_back(strip);
        }
    }
    return result;
}
}
std::map<std::string, ErLayoutPoint> ComputeErLayout(const std::vector<ErLayoutNode>& input, const std::vector<ErLayoutEdge>& edges) {
    if (input.empty()) return {};
    std::map<std::string, ErLayoutNode> normalized;
    for (const auto& node : input) {
        if (node.id.empty() || node.width <= 0 || node.height <= 0) throw std::invalid_argument("ER nodes require unique nonempty IDs and positive card sizes.");
        if (!normalized.emplace(node.id, node).second) throw std::invalid_argument("ER node IDs must be unique.");
    }
    std::vector<ErLayoutNode> nodes; std::map<std::string, Index> indices;
    for (const auto& item : normalized) { indices[item.first] = nodes.size(); nodes.push_back(item.second); }
    Graph children(nodes.size()), parents(nodes.size()); DisjointSets weak(nodes.size());
    for (const auto& edge : edges) {
        const auto source = indices.find(edge.source), target = indices.find(edge.target);
        if (source == indices.end() || target == indices.end()) continue;
        children[target->second].push_back(source->second); parents[source->second].push_back(target->second); weak.Join(source->second, target->second);
    }
    for (auto* graph : {&children, &parents}) for (auto& neighbors : *graph) { std::sort(neighbors.begin(), neighbors.end()); neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end()); }
    const auto strong = StrongComponents(children, parents); const auto layers = DependencyLayers(children, strong);
    std::map<Index, std::vector<Index>> weakMembers;
    for (Index node = 0; node < nodes.size(); ++node) weakMembers[weak.Find(node)].push_back(node);
    std::vector<int64_t> centers(nodes.size()); std::vector<int> y(nodes.size()); std::vector<Index> ranks(nodes.size()); std::vector<LocalLayout> layouts;
    for (const auto& component : weakMembers) layouts.push_back(ArrangeComponent(component.second, nodes, children, parents, strong, layers, centers, y, ranks));
    return Pack(std::move(layouts), nodes);
}
}
