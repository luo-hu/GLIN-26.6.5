#define main deli_benchmark_main
#include "../src/benchmark/bench_dynamic_compare_wkt.cpp"
#undef main

int main() {
  Options options;
  options.block_size = 1;
  options.local_delta_bound = 1;
  options.delete_compact_fraction = 0.25;
  options.cost_adaptive_partition = false;

  auto factory = geos::geom::GeometryFactory::create();
  geos::io::WKTReader reader(*factory);
  std::vector<GeometryPtr> geometries;
  std::vector<GeometryMeta> metadata;
  for (double z : {1.0, 2.0, 3.0, 0.5}) {
    GeometryPtr geometry(reader.read("POINT (0 0)"));
    GeometryMeta meta;
    meta.object_id = geometries.size();
    meta.zmin = z;
    meta.zmax = z;
    meta.envelope = *geometry->getEnvelopeInternal();
    metadata.push_back(meta);
    geometries.push_back(std::move(geometry));
  }

  LocalBoundedCompactQueryOverlay index(options, geometries, metadata);
  index.bulk_load({0, 1, 2});
  std::string reason;
  if (!index.erase(1) || !index.insert(3) ||
      !index.validate_index(&reason)) {
    std::cerr << "directory lost order after empty-middle-block reuse: "
              << reason << '\n';
    return 1;
  }
  return 0;
}
