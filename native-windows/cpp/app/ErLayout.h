#pragma once
#include <map>
#include <string>
#include <vector>

namespace native_app {
struct ErLayoutNode {
    std::string id;
    int width = 235;
    int height = 100;
};
// Source is the referencing table; target is the referenced parent table.
struct ErLayoutEdge { std::string source, target; };
struct ErLayoutPoint { int x = 0, y = 0; };

// Deterministic logical coordinates. Cycles share a dependency layer; saved
// manual positions are the caller's responsibility. Missing edge targets are ignored.
std::map<std::string, ErLayoutPoint> ComputeErLayout(
    const std::vector<ErLayoutNode>& nodes, const std::vector<ErLayoutEdge>& edges);
}
