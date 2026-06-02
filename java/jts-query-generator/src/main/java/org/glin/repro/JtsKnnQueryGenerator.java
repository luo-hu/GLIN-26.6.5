package org.glin.repro;

import org.locationtech.jts.geom.Envelope;
import org.locationtech.jts.geom.Geometry;
import org.locationtech.jts.index.strtree.GeometryItemDistance;
import org.locationtech.jts.index.strtree.STRtree;
import org.locationtech.jts.io.ParseException;
import org.locationtech.jts.io.WKTReader;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Random;

public final class JtsKnnQueryGenerator {
  private static final List<String> WKT_PREFIXES = Arrays.asList(
      "GEOMETRYCOLLECTION",
      "MULTIPOLYGON",
      "POLYGON",
      "MULTILINESTRING",
      "LINESTRING",
      "MULTIPOINT",
      "POINT");

  private static final List<Double> DEFAULT_SELECTIVITIES =
      Arrays.asList(0.01, 0.001, 0.0001, 0.00001);

  private static final class Options {
    String dataFile;
    String outputFile;
    String outputPrefix;
    long limit = 10_000;
    int queryCount = 100;
    long seed = 42;
    List<Double> selectivities = DEFAULT_SELECTIVITIES;
  }

  private static final class LoadResult {
    final List<Geometry> geometries = new ArrayList<>();
    long linesSeen = 0;
    long parseErrors = 0;
  }

  public static void main(String[] args) throws Exception {
    Options options = parseArgs(args);

    long loadStart = System.nanoTime();
    LoadResult loaded = loadWktFile(options);
    long loadEnd = System.nanoTime();
    if (loaded.geometries.isEmpty()) {
      throw new IllegalStateException("No valid geometries loaded");
    }

    long treeStart = System.nanoTime();
    STRtree tree = buildStrTree(loaded.geometries);
    long treeEnd = System.nanoTime();

    long queryStart = System.nanoTime();
    for (double selectivity : options.selectivities) {
      writeQueriesForSelectivity(options, loaded.geometries, tree, selectivity);
    }
    long queryEnd = System.nanoTime();

    System.out.printf(Locale.ROOT,
        "mode=jts_strtree_knn lines_seen=%d loaded=%d parse_errors=%d query_count=%d seed=%d load_ms=%.3f tree_ms=%.3f generate_ms=%.3f%n",
        loaded.linesSeen,
        loaded.geometries.size(),
        loaded.parseErrors,
        options.queryCount,
        options.seed,
        (loadEnd - loadStart) / 1e6,
        (treeEnd - treeStart) / 1e6,
        (queryEnd - queryStart) / 1e6);
  }

  private static Options parseArgs(String[] args) {
    Options options = new Options();
    for (int i = 0; i < args.length; i++) {
      String key = args[i];
      switch (key) {
        case "--data_file":
          options.dataFile = requireValue(args, ++i, key);
          break;
        case "--output_file":
          options.outputFile = requireValue(args, ++i, key);
          break;
        case "--output_prefix":
          options.outputPrefix = requireValue(args, ++i, key);
          break;
        case "--limit":
          options.limit = Long.parseLong(requireValue(args, ++i, key));
          break;
        case "--query_count":
          options.queryCount = Integer.parseInt(requireValue(args, ++i, key));
          break;
        case "--seed":
          options.seed = Long.parseLong(requireValue(args, ++i, key));
          break;
        case "--selectivities":
          options.selectivities = parseSelectivities(requireValue(args, ++i, key));
          break;
        case "--help":
        case "-h":
          printUsage();
          System.exit(0);
          break;
        default:
          throw new IllegalArgumentException("Unknown option: " + key);
      }
    }

    if (options.dataFile == null || options.dataFile.isEmpty()) {
      throw new IllegalArgumentException("--data_file is required");
    }
    if ((options.outputFile == null || options.outputFile.isEmpty())
        && (options.outputPrefix == null || options.outputPrefix.isEmpty())) {
      throw new IllegalArgumentException("--output_file or --output_prefix is required");
    }
    if (options.selectivities.size() > 1
        && options.outputPrefix == null
        && options.outputFile != null) {
      throw new IllegalArgumentException("Multiple selectivities need --output_prefix");
    }
    if (options.limit <= 0) {
      throw new IllegalArgumentException("--limit must be greater than 0");
    }
    if (options.queryCount <= 0) {
      throw new IllegalArgumentException("--query_count must be greater than 0");
    }
    return options;
  }

  private static String requireValue(String[] args, int index, String flag) {
    if (index >= args.length) {
      throw new IllegalArgumentException("Missing value for " + flag);
    }
    return args[index];
  }

  private static void printUsage() {
    System.err.println("Usage: mvn exec:java -Dexec.args=\"--data_file /path/to/data --output_prefix queries/name [options]\"");
    System.err.println("Options:");
    System.err.println("  --limit N             Number of valid geometries to load, default 10000");
    System.err.println("  --query_count N       Number of query windows per selectivity, default 100");
    System.err.println("  --selectivities LIST  Comma list such as 1%,0.1%,0.01%,0.001%");
    System.err.println("  --output_prefix PATH  Prefix for multiple output files");
    System.err.println("  --output_file PATH    Single output file, only for one selectivity");
    System.err.println("  --seed N              Random seed, default 42");
  }

  private static List<Double> parseSelectivities(String text) {
    List<Double> values = new ArrayList<>();
    for (String raw : text.split(",")) {
      String item = raw.trim();
      if (item.isEmpty()) {
        continue;
      }
      boolean percent = item.endsWith("%");
      if (percent) {
        item = item.substring(0, item.length() - 1);
      }
      double value = Double.parseDouble(item);
      if (percent) {
        value /= 100.0;
      }
      if (value <= 0.0 || value > 1.0) {
        throw new IllegalArgumentException("Selectivity must be in (0, 1], got: " + raw);
      }
      values.add(value);
    }
    if (values.isEmpty()) {
      throw new IllegalArgumentException("--selectivities produced an empty list");
    }
    return values;
  }

  private static LoadResult loadWktFile(Options options) throws IOException {
    WKTReader reader = new WKTReader();
    LoadResult result = new LoadResult();
    try (BufferedReader input = Files.newBufferedReader(Path.of(options.dataFile),
        StandardCharsets.UTF_8)) {
      String line;
      while (result.geometries.size() < options.limit && (line = input.readLine()) != null) {
        result.linesSeen++;
        String wkt = extractWkt(line);
        if (wkt.isEmpty()) {
          continue;
        }
        try {
          Geometry geometry = reader.read(wkt);
          if (geometry != null && !geometry.isEmpty()) {
            result.geometries.add(geometry);
          }
        } catch (ParseException | RuntimeException ex) {
          result.parseErrors++;
          if (result.parseErrors == 1) {
            System.err.println("First parse error at line " + result.linesSeen + ": "
                + ex.getMessage());
          }
        }
      }
    }
    return result;
  }

  private static String extractWkt(String line) {
    String text = stripBomAndCr(line);
    if (text.isEmpty()) {
      return "";
    }
    if (text.charAt(0) == '"') {
      int closeQuote = text.indexOf('"', 1);
      if (closeQuote > 0) {
        return text.substring(1, closeQuote);
      }
    }
    for (String prefix : WKT_PREFIXES) {
      int pos = text.indexOf(prefix);
      if (pos >= 0) {
        return extractBalancedWkt(text, pos);
      }
    }
    return text;
  }

  private static String stripBomAndCr(String line) {
    String text = line;
    if (!text.isEmpty() && text.charAt(text.length() - 1) == '\r') {
      text = text.substring(0, text.length() - 1);
    }
    if (text.length() >= 1 && text.charAt(0) == '\uFEFF') {
      text = text.substring(1);
    }
    return text;
  }

  private static String extractBalancedWkt(String text, int start) {
    int open = text.indexOf('(', start);
    if (open < 0) {
      return text.substring(start);
    }
    int depth = 0;
    for (int i = open; i < text.length(); i++) {
      char ch = text.charAt(i);
      if (ch == '(') {
        depth++;
      } else if (ch == ')') {
        depth--;
        if (depth == 0) {
          return text.substring(start, i + 1);
        }
      }
    }
    return text.substring(start);
  }

  private static STRtree buildStrTree(List<Geometry> geometries) {
    STRtree tree = new STRtree();
    for (Geometry geometry : geometries) {
      tree.insert(geometry.getEnvelopeInternal(), geometry);
    }
    tree.build();
    return tree;
  }

  private static void writeQueriesForSelectivity(
      Options options, List<Geometry> geometries, STRtree tree, double selectivity)
      throws IOException {
    int k = kForSelectivity(selectivity, geometries.size());
    String outputPath = outputPath(options, selectivity);
    Path output = Path.of(outputPath);
    if (output.getParent() != null) {
      Files.createDirectories(output.getParent());
    }

    Random random = new Random(options.seed);
    GeometryItemDistance distance = new GeometryItemDistance();
    try (BufferedWriter writer = Files.newBufferedWriter(output, StandardCharsets.UTF_8)) {
      writer.write("query_id,xmin,ymin,xmax,ymax,source_geometry_id,selectivity,k,mode");
      writer.newLine();
      for (int queryId = 0; queryId < options.queryCount; queryId++) {
        int sourceId = random.nextInt(geometries.size());
        Geometry source = geometries.get(sourceId);
        Object[] neighbours = tree.nearestNeighbour(
            source.getEnvelopeInternal(), source, distance, k);
        Envelope query = new Envelope(source.getEnvelopeInternal());
        for (Object item : neighbours) {
          Geometry neighbour = (Geometry) item;
          query.expandToInclude(neighbour.getEnvelopeInternal());
        }
        writer.write(String.format(Locale.ROOT,
            "%d,%.17g,%.17g,%.17g,%.17g,%d,%.17g,%d,jts_strtree_knn",
            queryId,
            query.getMinX(),
            query.getMinY(),
            query.getMaxX(),
            query.getMaxY(),
            sourceId,
            selectivity,
            k));
        writer.newLine();
      }
    }

    System.out.printf(Locale.ROOT,
        "query_file=%s selectivity=%.17g k=%d query_count=%d%n",
        outputPath,
        selectivity,
        k,
        options.queryCount);
  }

  private static int kForSelectivity(double selectivity, int loadedCount) {
    int k = (int) Math.ceil(selectivity * loadedCount);
    if (k < 1) {
      k = 1;
    }
    if (k > loadedCount) {
      k = loadedCount;
    }
    return k;
  }

  private static String outputPath(Options options, double selectivity) {
    if (options.outputPrefix != null && !options.outputPrefix.isEmpty()) {
      return options.outputPrefix + "_" + selectivityTag(selectivity) + ".csv";
    }
    return options.outputFile;
  }

  private static String selectivityTag(double selectivity) {
    if (selectivity >= 0.01) {
      return "1pct";
    }
    if (selectivity >= 0.001) {
      return "0p1pct";
    }
    if (selectivity >= 0.0001) {
      return "0p01pct";
    }
    if (selectivity >= 0.00001) {
      return "0p001pct";
    }
    return Double.toString(selectivity).replace('.', 'p').replace('-', 'm');
  }
}
