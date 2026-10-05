#include "ErRouting.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>

namespace native_app {
namespace {
constexpr int CoordinateLimit = 10000000;
constexpr std::size_t HardGridLimit = 250000, HardSearchLimit = 1000000;
using Cost = std::uint64_t;
struct Segment { ErRoutePoint a, b; };
bool IntersectsInterior(const Segment& line, const ErRouteRect& box) {
    if (line.a.y == line.b.y) return line.a.y > box.top && line.a.y < box.bottom && std::max(line.a.x, line.b.x) > box.left && std::min(line.a.x, line.b.x) < box.right;
    if (line.a.x == line.b.x) return line.a.x > box.left && line.a.x < box.right && std::max(line.a.y, line.b.y) > box.top && std::min(line.a.y, line.b.y) < box.bottom;
    return true;
}
std::vector<ErRoutePoint> Simplify(const std::vector<ErRoutePoint>& input) {
    std::vector<ErRoutePoint> output;
    for (const auto point : input) {
        if (!output.empty() && output.back() == point) continue;
        if (output.size() > 1) {
            const auto a = output[output.size() - 2], b = output.back();
            const bool horizontal = a.y == b.y && b.y == point.y && static_cast<std::int64_t>(b.x - a.x) * (point.x - b.x) >= 0;
            const bool vertical = a.x == b.x && b.x == point.x && static_cast<std::int64_t>(b.y - a.y) * (point.y - b.y) >= 0;
            if (horizontal || vertical) output.pop_back();
        }
        output.push_back(point);
    }
    return output;
}
struct Candidate {
    ErRoutePort source, target;
    std::vector<ErRoutePoint> points;
    Cost cost = std::numeric_limits<Cost>::max();
};
struct Grid {
    std::vector<int> xs, ys;
    std::vector<std::uint8_t> blocked, blockedH, blockedV, usedH, usedV, usedAt;
    std::vector<Cost> distance;
    std::vector<std::uint32_t> parent, generation;
    std::uint32_t serial = 0;
    std::size_t nx = 0, ny = 0;
    std::size_t Node(int x, int y) const { return static_cast<std::size_t>(std::lower_bound(ys.begin(), ys.end(), y) - ys.begin()) * nx + static_cast<std::size_t>(std::lower_bound(xs.begin(), xs.end(), x) - xs.begin()); }
    ErRoutePoint Point(std::size_t node) const { return {xs[node % nx], ys[node / nx]}; }
    void Build(const std::vector<ErRouteRect>& obstacles) {
        auto unique = [](std::vector<int>& values) { std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end()); };
        unique(xs); unique(ys); nx = xs.size(); ny = ys.size();
        const auto count = nx * ny;
        blocked.resize(count); blockedH.resize(count); blockedV.resize(count); usedH.resize(count); usedV.resize(count); usedAt.resize(count);
        distance.resize(count * 2); parent.resize(count * 2); generation.resize(count * 2);
        for (const auto& box : obstacles) {
            const auto x0 = static_cast<std::size_t>(std::upper_bound(xs.begin(), xs.end(), box.left) - xs.begin()), x1 = static_cast<std::size_t>(std::lower_bound(xs.begin(), xs.end(), box.right) - xs.begin());
            const auto y0 = static_cast<std::size_t>(std::upper_bound(ys.begin(), ys.end(), box.top) - ys.begin()), y1 = static_cast<std::size_t>(std::lower_bound(ys.begin(), ys.end(), box.bottom) - ys.begin());
            const auto hx0 = static_cast<std::size_t>(std::lower_bound(xs.begin(), xs.end(), box.left) - xs.begin()), hx1 = static_cast<std::size_t>(std::lower_bound(xs.begin(), xs.end(), box.right) - xs.begin());
            const auto vy0 = static_cast<std::size_t>(std::lower_bound(ys.begin(), ys.end(), box.top) - ys.begin()), vy1 = static_cast<std::size_t>(std::lower_bound(ys.begin(), ys.end(), box.bottom) - ys.begin());
            for (std::size_t y = y0; y < y1; ++y) {
                std::fill(blocked.begin() + y * nx + x0, blocked.begin() + y * nx + x1, std::uint8_t{1});
                std::fill(blockedH.begin() + y * nx + hx0, blockedH.begin() + y * nx + hx1, std::uint8_t{1});
            }
            for (std::size_t y = vy0; y < vy1; ++y) std::fill(blockedV.begin() + y * nx + x0, blockedV.begin() + y * nx + x1, std::uint8_t{1});
        }
    }
    Cost Step(std::size_t from, std::size_t to, unsigned direction, const ErRoutingOptions& options) const {
        const auto a = Point(from), b = Point(to); const auto length = static_cast<Cost>(std::abs(a.x - b.x) + std::abs(a.y - b.y));
        const auto edge = std::min(from, to); const auto uses = direction == 0 ? usedH[edge] : usedV[edge];
        const bool crossing = (usedAt[to] & (direction == 0 ? 2 : 1)) != 0;
        return length * (1 + static_cast<Cost>(options.sharedSegmentPenalty) * std::min<unsigned>(uses, 3)) + (crossing ? options.crossingPenalty : 0);
    }
    bool Available(std::size_t from, std::size_t to, unsigned direction) const {
        return !blocked[to] && !(direction == 0 ? blockedH[std::min(from, to)] : blockedV[std::min(from, to)]);
    }
    std::vector<ErRoutePoint> Search(ErRoutePoint from, ErRoutePoint to, const ErRoutingOptions& options,
        ErRoutingResult& result, Cost& cost) {
        const auto start = Node(from.x, from.y), goal = Node(to.x, to.y);
        if (blocked[start] || blocked[goal]) return {};
        struct Entry { Cost estimate, cost; std::uint32_t state; };
        struct Later { bool operator()(const Entry& a, const Entry& b) const { return std::tie(a.estimate, a.cost, a.state) > std::tie(b.estimate, b.cost, b.state); } };
        std::priority_queue<Entry, std::vector<Entry>, Later> pending;
        ++serial; const auto initial = static_cast<std::uint32_t>(start * 2);
        generation[initial] = serial; distance[initial] = 0; parent[initial] = initial;
        auto heuristic = [&](std::size_t node) { const auto point = Point(node); return static_cast<Cost>(std::abs(point.x - to.x) + std::abs(point.y - to.y)); };
        pending.push({heuristic(start), 0, initial});
        Cost best = std::numeric_limits<Cost>::max(); std::uint32_t final = initial; std::size_t expanded = 0;
        while (!pending.empty()) {
            const auto entry = pending.top(); pending.pop();
            if (generation[entry.state] != serial || distance[entry.state] != entry.cost) continue;
            if (entry.estimate >= best) break;
            if (expanded >= options.maximumExpandedStatesPerSearch || result.expandedStates >= options.maximumExpandedStatesTotal) { result.limited = true; break; }
            ++expanded; ++result.expandedStates;
            const auto node = entry.state / 2, previous = entry.state % 2;
            if (node == goal) { const auto finished = entry.cost + (previous ? options.bendPenalty : 0); if (finished < best) { best = finished; final = entry.state; } continue; }
            const auto x = node % nx, y = node / nx;
            const std::array<std::pair<std::size_t, unsigned>, 4> neighbors{{{x ? node - 1 : node, 0}, {x + 1 < nx ? node + 1 : node, 0}, {y ? node - nx : node, 1}, {y + 1 < ny ? node + nx : node, 1}}};
            for (const auto neighbor : neighbors) {
                if (neighbor.first == node || !Available(node, neighbor.first, neighbor.second)) continue;
                const auto state = static_cast<std::uint32_t>(neighbor.first * 2 + neighbor.second);
                const auto next = entry.cost + Step(node, neighbor.first, neighbor.second, options) + (previous == neighbor.second ? 0 : options.bendPenalty);
                if (generation[state] != serial || next < distance[state]) {
                    generation[state] = serial; distance[state] = next; parent[state] = entry.state; pending.push({next + heuristic(neighbor.first), next, state});
                }
            }
        }
        if (best == std::numeric_limits<Cost>::max()) return {};
        std::vector<ErRoutePoint> points;
        for (auto state = final;; state = parent[state]) { points.push_back(Point(state / 2)); if (state == initial) break; }
        std::reverse(points.begin(), points.end()); cost = best; return points;
    }
    Cost PathCost(const std::vector<ErRoutePoint>& points, const ErRoutingOptions& options, bool record) {
        Cost cost = 0; int previous = -1;
        for (std::size_t i = 1; i < points.size(); ++i) {
            const auto from = Node(points[i - 1].x, points[i - 1].y), to = Node(points[i].x, points[i].y);
            const unsigned direction = points[i - 1].y == points[i].y ? 0 : 1;
            if (previous >= 0 && previous != static_cast<int>(direction)) cost += options.bendPenalty;
            previous = static_cast<int>(direction);
            const auto step = direction == 0 ? std::size_t(1) : nx;
            for (auto node = std::min(from, to); node < std::max(from, to); node += step) {
                cost += Step(node, node + step, direction, options);
                if (record) { auto& uses = direction == 0 ? usedH[node] : usedV[node]; if (uses < 255) ++uses; usedAt[node] |= direction == 0 ? 1 : 2; usedAt[node + step] |= direction == 0 ? 1 : 2; }
            }
        }
        return cost;
    }
};
}

ErRoutingResult RouteErConnections(const std::vector<ErRouteCard>& cards,
    const std::vector<ErRouteConnection>& connections, const ErRoutingOptions& requested) {
    ErRoutingResult result; result.routes.resize(connections.size());
    auto failAll = [&](const char* reason) { for (auto& route : result.routes) route.failure = reason; result.limited = true; };
    auto options = requested;
    options.maximumCards = std::min<std::size_t>(options.maximumCards, 256); options.maximumConnections = std::min<std::size_t>(options.maximumConnections, 1024);
    options.maximumGridVertices = std::min(options.maximumGridVertices, HardGridLimit);
    options.maximumExpandedStatesPerSearch = std::min(options.maximumExpandedStatesPerSearch, HardSearchLimit);
    options.maximumExpandedStatesTotal = std::min(options.maximumExpandedStatesTotal, HardSearchLimit);
    if (options.clearance < 1 || options.clearance > 1024 || options.laneSpacing < 1 || options.laneSpacing > 1024 || options.bendPenalty < 0 || options.bendPenalty > 10000 || options.sharedSegmentPenalty < 0 || options.sharedSegmentPenalty > 1024 || options.crossingPenalty < 0 || options.crossingPenalty > 10000) { failAll("Invalid routing options."); return result; }
    if (cards.size() > options.maximumCards || connections.size() > options.maximumConnections) {
        for (const auto& card : cards) { result.extentRight = std::max(result.extentRight, card.bounds.right); result.extentBottom = std::max(result.extentBottom, card.bounds.bottom); }
        failAll("Diagram exceeds the routing card or connection limit."); return result;
    }
    std::map<std::string, std::size_t> indices;
    std::vector<ErRouteRect> obstacles; obstacles.reserve(cards.size()); Grid grid;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        const auto& card = cards[i]; const auto& box = card.bounds;
        if (card.id.empty() || !indices.emplace(card.id, i).second || box.left < 0 || box.top < 0 || box.right <= box.left || box.bottom <= box.top || box.right > CoordinateLimit || box.bottom > CoordinateLimit) { failAll("Invalid or duplicate card geometry."); return result; }
        result.extentRight = std::max(result.extentRight, box.right); result.extentBottom = std::max(result.extentBottom, box.bottom);
        obstacles.push_back({box.left - options.clearance, box.top - options.clearance, box.right + options.clearance, box.bottom + options.clearance});
        grid.xs.push_back(box.left); grid.xs.push_back(box.right);
        for (int lane = 0; lane <= 3; ++lane) {
            grid.xs.push_back(std::max(0, box.left - options.clearance - lane * options.laneSpacing)); grid.xs.push_back(box.right + options.clearance + lane * options.laneSpacing);
            grid.ys.push_back(std::max(0, box.top - options.clearance - lane * options.laneSpacing)); grid.ys.push_back(box.bottom + options.clearance + lane * options.laneSpacing);
        }
    }
    if (connections.empty()) return result;
    auto row = [&](const ErRouteCard& card, int offset) { return card.bounds.top + offset; };
    for (std::size_t i = 0; i < connections.size(); ++i) {
        const auto& edge = connections[i]; const auto source = indices.find(edge.sourceId), target = indices.find(edge.targetId);
        if (source == indices.end() || target == indices.end()) { result.routes[i].failure = "Relationship endpoint is missing."; continue; }
        const auto& a = cards[source->second]; const auto& b = cards[target->second];
        if (edge.sourceRowOffset < 0 || edge.sourceRowOffset > a.bounds.bottom - a.bounds.top || edge.targetRowOffset < 0 || edge.targetRowOffset > b.bounds.bottom - b.bounds.top) { result.routes[i].failure = "Relationship row is outside its card."; continue; }
        for (const auto y : {row(a, edge.sourceRowOffset), row(b, edge.targetRowOffset)}) for (int lane = -3; lane <= 3; ++lane) grid.ys.push_back(std::max(0, y + lane * options.laneSpacing));
    }
    grid.xs.push_back(0); grid.ys.push_back(0); grid.xs.push_back(result.extentRight + options.clearance + options.laneSpacing * 4); grid.ys.push_back(result.extentBottom + options.clearance + options.laneSpacing * 4);
    auto unique = [](std::vector<int>& values) { std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end()); };
    unique(grid.xs); unique(grid.ys);
    if (grid.xs.size() > options.maximumGridVertices / grid.ys.size()) { failAll("Diagram exceeds the routing grid limit."); return result; }
    grid.Build(obstacles);
    auto clear = [&](const std::vector<ErRoutePoint>& points, std::size_t source, std::size_t target) {
        for (const auto point : points) if (point.x < 0 || point.y < 0) return false;
        for (std::size_t i = 1; i < points.size(); ++i) for (std::size_t card = 0; card < obstacles.size(); ++card) {
            if ((i == 1 && card == source) || (i + 1 == points.size() && card == target)) continue;
            if (IntersectsInterior({points[i - 1], points[i]}, obstacles[card])) return false;
        }
        return true;
    };
    std::vector<std::size_t> order(connections.size()); std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const auto& x = connections[a]; const auto& y = connections[b];
        return std::tie(x.sourceId, x.targetId, x.sourceRowOffset, x.targetRowOffset) < std::tie(y.sourceId, y.targetId, y.sourceRowOffset, y.targetRowOffset);
    });
    for (const auto edgeIndex : order) {
        auto& output = result.routes[edgeIndex]; if (!output.failure.empty()) continue;
        if (result.expandedStates >= options.maximumExpandedStatesTotal) { output.failure = "Routing search budget exhausted."; result.limited = true; continue; }
        const auto& edge = connections[edgeIndex]; const auto sourceIndex = indices.at(edge.sourceId), targetIndex = indices.at(edge.targetId);
        const auto& sourceCard = cards[sourceIndex]; const auto& targetCard = cards[targetIndex]; Candidate best;
        for (const auto sourceSide : {ErRouteSide::Right, ErRouteSide::Left}) for (const auto targetSide : {ErRouteSide::Left, ErRouteSide::Right}) {
            const ErRoutePort source{{sourceSide == ErRouteSide::Left ? sourceCard.bounds.left : sourceCard.bounds.right, row(sourceCard, edge.sourceRowOffset)}, sourceSide};
            const ErRoutePort target{{targetSide == ErRouteSide::Left ? targetCard.bounds.left : targetCard.bounds.right, row(targetCard, edge.targetRowOffset)}, targetSide};
            const ErRoutePoint start{source.point.x + (sourceSide == ErRouteSide::Left ? -options.clearance : options.clearance), source.point.y};
            const ErRoutePoint goal{target.point.x + (targetSide == ErRouteSide::Left ? -options.clearance : options.clearance), target.point.y};
            if (start.x < 0 || goal.x < 0 || !clear({source.point, start}, sourceIndex, sourceIndex) || !clear({goal, target.point}, targetIndex, targetIndex)) continue;
            if (start == goal && sourceIndex == targetIndex) {
                for (int lane = 1; lane <= 3; ++lane) for (const int sign : {-1, 1}) {
                    const auto outer = start.x + (sourceSide == ErRouteSide::Left ? -1 : 1) * lane * options.laneSpacing;
                    const auto y = start.y + sign * lane * options.laneSpacing;
                    const auto points = Simplify({source.point, start, {outer, start.y}, {outer, y}, {start.x, y}, start, target.point});
                    if (!clear(points, sourceIndex, targetIndex)) continue;
                    const auto cost = grid.PathCost(points, options, false);
                    if (cost < best.cost) best = {source, target, points, cost};
                }
                continue;
            }
            Cost searchCost = 0; const auto path = grid.Search(start, goal, options, result, searchCost);
            if (path.empty()) continue;
            std::vector<ErRoutePoint> points{source.point}; points.insert(points.end(), path.begin(), path.end()); points.push_back(target.point); points = Simplify(points);
            if (!clear(points, sourceIndex, targetIndex)) continue;
            const auto cost = grid.PathCost(points, options, false);
            if (cost < best.cost) best = {source, target, std::move(points), cost};
        }
        if (best.points.empty()) { output.failure = result.expandedStates >= options.maximumExpandedStatesTotal ? "Routing search budget exhausted." : "No connector route with the requested card clearance."; continue; }
        output.routed = true; output.source = best.source; output.target = best.target; output.points = std::move(best.points);
        grid.PathCost(output.points, options, true);
        for (const auto point : output.points) { result.extentRight = std::max(result.extentRight, point.x); result.extentBottom = std::max(result.extentBottom, point.y); }
    }
    return result;
}
} // namespace native_app
