#ifndef GLIN_RECTANGLE_PREDICATE_SHORTCUT_H
#define GLIN_RECTANGLE_PREDICATE_SHORTCUT_H

#include <geos/geom/Coordinate.h>
#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>

namespace deli_fusion {

enum class RectangleShortcutKind {
  None = 0,
  EnvelopeContains = 1,
  GeometryVertex = 2,
};

inline bool envelope_contains(const geos::geom::Envelope& outer,
                              const geos::geom::Envelope& inner) {
  return outer.getMinX() <= inner.getMinX() &&
         outer.getMaxX() >= inner.getMaxX() &&
         outer.getMinY() <= inner.getMinY() &&
         outer.getMaxY() >= inner.getMaxY();
}

inline RectangleShortcutKind rectangle_query_candidate_shortcut(
    bool shortcuts_enabled, bool geometry_vertex_witness_enabled,
    const geos::geom::Envelope& query_envelope,
    const geos::geom::Envelope& candidate_envelope,
    const geos::geom::Geometry* candidate) {
  if (!shortcuts_enabled) {
    return RectangleShortcutKind::None;
  }
  if (envelope_contains(query_envelope, candidate_envelope)) {
    return RectangleShortcutKind::EnvelopeContains;
  }
  const geos::geom::Coordinate* vertex =
      geometry_vertex_witness_enabled && candidate != nullptr
          ? candidate->getCoordinate()
          : nullptr;
  if (vertex != nullptr && vertex->x >= query_envelope.getMinX() &&
      vertex->x <= query_envelope.getMaxX() &&
      vertex->y >= query_envelope.getMinY() &&
      vertex->y <= query_envelope.getMaxY()) {
    return RectangleShortcutKind::GeometryVertex;
  }
  return RectangleShortcutKind::None;
}

}  // namespace deli_fusion

#endif
