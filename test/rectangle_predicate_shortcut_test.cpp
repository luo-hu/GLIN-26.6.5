#include "../src/benchmark/rectangle_predicate_shortcut.h"

#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKTReader.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using deli_fusion::RectangleShortcutKind;
using GeometryPtr = std::unique_ptr<geos::geom::Geometry>;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

GeometryPtr read_geometry(geos::io::WKTReader& reader,
                          const std::string& wkt) {
  return GeometryPtr(reader.read(wkt));
}

RectangleShortcutKind shortcut(const geos::geom::Envelope& query,
                               const geos::geom::Geometry* candidate,
                               bool enabled = true,
                               bool vertex_enabled = true) {
  return deli_fusion::rectangle_query_candidate_shortcut(
      enabled, vertex_enabled, query, *candidate->getEnvelopeInternal(),
      candidate);
}

void test_envelope_containment(geos::io::WKTReader& reader) {
  const geos::geom::Envelope query(0.0, 2.0, 0.0, 2.0);
  GeometryPtr point = read_geometry(reader, "POINT (1 1)");
  require(shortcut(query, point.get()) ==
              RectangleShortcutKind::EnvelopeContains,
          "contained point did not use envelope shortcut");
}

void test_geometry_vertex_witness(geos::io::WKTReader& reader) {
  const geos::geom::Envelope query(0.0, 2.0, 0.0, 2.0);
  GeometryPtr line = read_geometry(reader, "LINESTRING (1 1, 10 10)");
  require(shortcut(query, line.get()) ==
              RectangleShortcutKind::GeometryVertex,
          "line vertex did not prove intersection");
  require(shortcut(query, line.get(), true, false) ==
              RectangleShortcutKind::None,
          "disabled geometry-vertex witness was still used");

  GeometryPtr boundary_line =
      read_geometry(reader, "LINESTRING (0 2, -10 10)");
  require(shortcut(query, boundary_line.get()) ==
              RectangleShortcutKind::GeometryVertex,
          "query-boundary vertex was not accepted");

  GeometryPtr concave = read_geometry(
      reader,
      "POLYGON ((1 1, 8 1, 8 8, 4 8, 4 3, 1 3, 1 1))");
  require(shortcut(query, concave.get()) ==
              RectangleShortcutKind::GeometryVertex,
          "concave polygon vertex did not prove intersection");

  GeometryPtr collection = read_geometry(
      reader,
      "GEOMETRYCOLLECTION (LINESTRING (1 1, 10 10), POINT (20 20))");
  require(shortcut(query, collection.get()) ==
              RectangleShortcutKind::GeometryVertex,
          "geometry collection vertex did not prove intersection");
}

void test_hole_and_empty_are_not_false_accepts(
    geos::io::WKTReader& reader) {
  const geos::geom::Envelope hole_query(4.5, 5.5, 4.5, 5.5);
  GeometryPtr polygon_with_hole = read_geometry(
      reader,
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(4 4, 4 6, 6 6, 6 4, 4 4))");
  require(shortcut(hole_query, polygon_with_hole.get()) ==
              RectangleShortcutKind::None,
          "polygon hole produced an unsafe positive shortcut");

  GeometryPtr empty = read_geometry(reader, "POLYGON EMPTY");
  require(shortcut(hole_query, empty.get()) == RectangleShortcutKind::None,
          "empty geometry produced a shortcut");
}

void test_global_disable(geos::io::WKTReader& reader) {
  const geos::geom::Envelope query(0.0, 2.0, 0.0, 2.0);
  GeometryPtr point = read_geometry(reader, "POINT (1 1)");
  require(shortcut(query, point.get(), false, true) ==
              RectangleShortcutKind::None,
          "disabled PRL still accepted a candidate");
}

}  // namespace

int main() {
  try {
    geos::geom::GeometryFactory::Ptr factory =
        geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);
    test_envelope_containment(reader);
    test_geometry_vertex_witness(reader);
    test_hole_and_empty_are_not_false_accepts(reader);
    test_global_disable(reader);
    std::cout << "rectangle predicate shortcut tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rectangle predicate shortcut test failed: " << error.what()
              << "\n";
    return 1;
  }
}
