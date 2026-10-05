#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace native_app {
struct ErRoutePoint {
    int x = 0, y = 0;
    bool operator==(const ErRoutePoint& other) const { return x == other.x && y == other.y; }
};
struct ErRouteRect { int left = 0, top = 0, right = 0, bottom = 0; };
struct ErRouteCard { std::string id; ErRouteRect bounds; };
struct ErRouteConnection {
    std::string sourceId, targetId;
    // Logical distances down from each card's top edge; row centers stay exact.
    int sourceRowOffset = 0, targetRowOffset = 0;
};
enum class ErRouteSide { Left, Right };
struct ErRoutePort { ErRoutePoint point; ErRouteSide side = ErRouteSide::Right; };
struct ErConnectorRoute {
    bool routed = false;
    ErRoutePort source, target;
    std::vector<ErRoutePoint> points;
    std::string failure;
};
struct ErRoutingOptions {
    int clearance = 12, laneSpacing = 10, bendPenalty = 32;
    int sharedSegmentPenalty = 8, crossingPenalty = 160;
    std::size_t maximumCards = 128, maximumConnections = 512;
    std::size_t maximumGridVertices = 180000;
    std::size_t maximumExpandedStatesPerSearch = 12000;
    std::size_t maximumExpandedStatesTotal = 250000;
};
struct ErRoutingResult {
    // Same order and number as the input connections. Unresolved routes have no points.
    std::vector<ErConnectorRoute> routes;
    int extentRight = 0, extentBottom = 0;
    std::size_t expandedStates = 0;
    bool limited = false;
};

// Deterministic, nonnegative logical coordinates; no Win32 or renderer dependency.
// Search uses a compressed orthogonal grid and enforces both per-search and batch
// expansion limits. A failure never returns a line that crosses a card.
ErRoutingResult RouteErConnections(const std::vector<ErRouteCard>& cards,
    const std::vector<ErRouteConnection>& connections, const ErRoutingOptions& options = {});
} // namespace native_app
