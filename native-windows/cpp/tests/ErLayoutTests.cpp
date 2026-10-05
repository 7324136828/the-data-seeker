#include "ErLayout.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using namespace native_app;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool Same(const std::map<std::string, ErLayoutPoint>& a, const std::map<std::string, ErLayoutPoint>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& item : a) { const auto other = b.find(item.first); if (other == b.end() || item.second.x != other->second.x || item.second.y != other->second.y) return false; }
    return true;
}
void NoOverlap(const std::vector<ErLayoutNode>& nodes, const std::map<std::string, ErLayoutPoint>& positions) {
    Require(nodes.size() == positions.size(), "ER layout lost or invented nodes.");
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto a = positions.at(nodes[i].id); Require(a.x >= 40 && a.y >= 40, "ER layout did not leave a positive routing margin.");
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            const auto b = positions.at(nodes[j].id);
            const bool overlap = static_cast<int64_t>(a.x) < static_cast<int64_t>(b.x) + nodes[j].width
                && static_cast<int64_t>(b.x) < static_cast<int64_t>(a.x) + nodes[i].width
                && static_cast<int64_t>(a.y) < static_cast<int64_t>(b.y) + nodes[j].height
                && static_cast<int64_t>(b.y) < static_cast<int64_t>(a.y) + nodes[i].height;
            Require(!overlap, "ER cards overlap.");
        }
    }
}
uint64_t Crossings(const std::vector<ErLayoutEdge>& edges, const std::map<std::string, ErLayoutPoint>& points) {
    uint64_t result = 0;
    for (size_t i = 0; i < edges.size(); ++i) for (size_t j = i + 1; j < edges.size(); ++j) {
        if (edges[i].source == edges[j].source || edges[i].target == edges[j].target) continue;
        const auto parentA = points.at(edges[i].target), parentB = points.at(edges[j].target);
        const auto childA = points.at(edges[i].source), childB = points.at(edges[j].source);
        if (parentA.x == parentB.x && childA.x == childB.x
            && ((parentA.y < parentB.y && childA.y > childB.y) || (parentA.y > parentB.y && childA.y < childB.y))) ++result;
    }
    return result;
}
}
void TestErLayout() {
    Require(ComputeErLayout({}, {}).empty(), "Empty ER layout must be empty.");
    std::vector<ErLayoutNode> nodes{{"orders",235,200},{"customers",235,100},{"items",235,380},{"products",235,180},{"reviews",235,260}};
    std::vector<ErLayoutEdge> edges{{"orders","customers"},{"items","orders"},{"items","products"},{"reviews","customers"},{"reviews","products"}};
    auto layout = ComputeErLayout(nodes, edges); NoOverlap(nodes, layout);
    for (const auto& edge : edges) Require(layout.at(edge.target).x + 235 + 80 <= layout.at(edge.source).x, "Referenced parents must occupy layers to the left of their dependents.");
    Require(layout.at("items").x > layout.at("orders").x && layout.at("orders").x > layout.at("customers").x, "Multi-level dependencies lost their longest-path layers.");
    std::reverse(nodes.begin(), nodes.end()); std::reverse(edges.begin(), edges.end());
    Require(Same(layout, ComputeErLayout(nodes, edges)), "ER layout changes with input ordering.");
    edges.push_back(edges.front()); edges.push_back({"orders","missing-table"});
    Require(Same(layout, ComputeErLayout(nodes, edges)), "Duplicate or unavailable foreign-key targets changed the ER layout.");

    nodes = {{"a",180,580},{"b",280,70},{"c",235,230},{"child",310,420},{"parent",160,120}};
    edges = {{"a","b"},{"b","c"},{"c","a"},{"child","b"},{"a","parent"},{"b","b"}};
    layout = ComputeErLayout(nodes, edges); NoOverlap(nodes, layout);
    const std::vector<int> cycleCenters{layout.at("a").x*2+nodes[0].width,layout.at("b").x*2+nodes[1].width,layout.at("c").x*2+nodes[2].width};
    Require(*std::max_element(cycleCenters.begin(),cycleCenters.end())-*std::min_element(cycleCenters.begin(),cycleCenters.end())<=1,
        "Cycle members must share a finite dependency layer.");
    Require(layout.at("parent").x < layout.at("a").x && layout.at("child").x > layout.at("b").x, "Cycle condensation lost external parent/child ordering.");
    std::reverse(nodes.begin(), nodes.end()); std::reverse(edges.begin(), edges.end());
    Require(Same(layout, ComputeErLayout(nodes, edges)), "Cyclic ER layout is not deterministic.");

    nodes = {{"a",235,100},{"b",235,100},{"c",235,100},{"x",235,100},{"y",235,100},{"z",235,100}};
    edges = {{"z","a"},{"y","a"},{"y","b"},{"y","c"},{"x","c"}};
    std::map<std::string,ErLayoutPoint> alphabetical{{"a",{40,40}},{"b",{40,188}},{"c",{40,336}},{"x",{371,40}},{"y",{371,188}},{"z",{371,336}}};
    layout = ComputeErLayout(nodes, edges); NoOverlap(nodes, layout);
    Require(Crossings(edges, layout) < Crossings(edges, alphabetical), "Barycenter ordering did not reduce a real crossing fixture.");
    nodes.clear(); edges.clear();
    for (int layer = 0; layer < 4; ++layer) for (int row = 0; row < 6; ++row) nodes.push_back({"layer"+std::to_string(layer)+"_"+std::to_string(row),180+row*23,90+row*45});
    for (int layer = 1; layer < 4; ++layer) for (int child = 0; child < 6; ++child) for (int parent = 0; parent < 6; ++parent)
        if ((child+parent)%2==0 || parent==0) edges.push_back({"layer"+std::to_string(layer)+"_"+std::to_string(child),"layer"+std::to_string(layer-1)+"_"+std::to_string(parent)});
    layout = ComputeErLayout(nodes, edges); NoOverlap(nodes, layout);
    for (const auto& edge : edges) Require(layout.at(edge.target).x < layout.at(edge.source).x, "Dense graph reversed a dependency.");
    std::reverse(nodes.begin(), nodes.end()); std::reverse(edges.begin(), edges.end());
    Require(Same(layout, ComputeErLayout(nodes, edges)), "Dense barycenter layout is not stable.");

    nodes.clear(); edges.clear();
    for (int i = 0; i < 15; ++i) nodes.push_back({"isolated_"+std::to_string(i),235,100+(i%3)*45});
    layout = ComputeErLayout(nodes, edges); NoOverlap(nodes, layout); std::set<int> columns, rows;
    int right = 0;
    for (const auto& node : nodes) { const auto position = layout.at(node.id); columns.insert(position.x); rows.insert(position.y); right = std::max(right, position.x + node.width); }
    Require(columns.size() >= 2 && rows.size() >= 2 && right <= 1540, "Disconnected cards were not distributed compactly.");
    nodes.push_back({"tall",235,1000}); nodes.push_back({"leaf",235,90}); edges.push_back({"leaf","tall"});
    layout = ComputeErLayout(nodes, edges); NoOverlap(nodes, layout);

    // Iterative SCC traversal must also work for a schema deeper than a native call stack.
    nodes.clear(); edges.clear();
    for (int i = 0; i < 4000; ++i) { nodes.push_back({"cycle_"+std::to_string(i),235,50}); edges.push_back({"cycle_"+std::to_string(i),"cycle_"+std::to_string((i+1)%4000)}); }
    layout = ComputeErLayout(nodes, edges); Require(layout.size()==nodes.size(), "Deep cycle did not terminate.");
    std::vector<int> starts; for (const auto& node : nodes) { const auto position=layout.at(node.id); Require(position.x==40, "One SCC escaped its layer."); starts.push_back(position.y); }
    std::sort(starts.begin(),starts.end()); for (size_t i=1;i<starts.size();++i) Require(starts[i]-starts[i-1]>=98,"Deep SCC cards overlap.");
    bool rejected=false;
    try { ComputeErLayout({{"duplicate",235,100},{"duplicate",235,100}},{}); } catch (const std::invalid_argument&) { rejected=true; }
    Require(rejected,"Duplicate ER node IDs were silently discarded.");
    rejected=false; try { ComputeErLayout({{"huge",std::numeric_limits<int>::max(),100}},{}); } catch (const std::overflow_error&) { rejected=true; }
    Require(rejected,"ER coordinate overflow was not rejected.");
}
