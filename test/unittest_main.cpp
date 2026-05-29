// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// These Boost.Geometry headers must be parsed before the test-only
// private/public macro below, otherwise Boost internals fail to compile.
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

#define private public
#include "unittest_alex.h"
#include "unittest_alex_map.h"
#include "unittest_alex_multimap.h"
#include "unittest_geos.h"
#include "unittest_nodes.h"
#include "unittest_glin.h"
#include "unittest_glin_insert.h"
#include "unittest_glin_piecewise.h"
#include "unittest_glin_line_projct.h"
