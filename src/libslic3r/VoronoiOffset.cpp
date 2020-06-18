// Polygon offsetting using Voronoi diagram prodiced by boost::polygon.

#include "VoronoiOffset.hpp"
#include "libslic3r.h"

#include <cmath>

// #define VORONOI_DEBUG_OUT

#ifdef VORONOI_DEBUG_OUT
#include <libslic3r/VoronoiVisualUtils.hpp>
#endif

namespace Slic3r {
namespace Voronoi {

namespace detail {
    // Intersect a circle with a ray, return the two parameters.
    // Currently used for unbounded Voronoi edges only.
	double first_circle_segment_intersection_parameter(
		const Vec2d &center, const double r, const Vec2d &pt, const Vec2d &v)
	{
		const Vec2d 	d = pt - center;
#ifndef NDEBUG
        double          d0 = (pt - center).norm();
        double          d1 = (pt + v - center).norm();
        assert(r < std::max(d0, d1) + EPSILON);
#endif /* NDEBUG */
        const double	a = v.squaredNorm();
		const double 	b = 2. * d.dot(v);
		const double    c = d.squaredNorm() - r * r;
		std::pair<int, std::array<double, 2>> out;
        double          u = b * b - 4. * a * c;
		assert(u > - EPSILON);
		double          t;
		if (u <= 0) {
			// Degenerate to a single closest point.
			t = - b / (2. * a);
			assert(t >= - EPSILON && t <= 1. + EPSILON);
			return Slic3r::clamp(0., 1., t);
		} else {
			u = sqrt(u);
			out.first = 2;
			double t0 = (- b - u) / (2. * a);
			double t1 = (- b + u) / (2. * a);
			// One of the intersections shall be found inside the segment.
			assert((t0 >= - EPSILON && t0 <= 1. + EPSILON) || (t1 >= - EPSILON && t1 <= 1. + EPSILON));
			if (t1 < 0.)
				return 0.;
			if (t0 > 1.)
				return 1.;
			return (t0 > 0.) ? t0 : t1;
		}
	}

    struct Intersections
    {
        int     count;
        Vec2d   pts[2];
    };

    // Return maximum two points, that are at distance "d" from both points
    Intersections point_point_equal_distance_points(const Point &pt1, const Point &pt2, const double d)
    {
        // Calculate the two intersection points.
        // With the help of Python package sympy:
        //      res = solve([(x - cx)**2 + (y - cy)**2 - d**2, x**2 + y**2 - d**2], [x, y])
        //      ccode(cse((res[0][0], res[0][1], res[1][0], res[1][1])))
        // where cx, cy is the center of pt1 relative to pt2,
        // d is distance from the line and the point (0, 0).
        // The result is then shifted to pt2.
        auto   cx = double(pt1.x() - pt2.x());
        auto   cy = double(pt1.y() - pt2.y());
        double cl = cx * cx + cy * cy;
        double discr = 4. * d * d - cl;
        if (discr < 0.) {
            // No intersection point found, the two circles are too far away.
            return Intersections { 0, { Vec2d(), Vec2d() } };
        }
        // Avoid division by zero if a gets too small.
        bool   xy_swapped = std::abs(cx) < std::abs(cy);
        if (xy_swapped)
            std::swap(cx, cy);
        double u;
        int    cnt;
        if (discr == 0.) {
            cnt = 1;
            u   = 0;
        } else {
            cnt = 2;
            u = 0.5 * cx * sqrt(cl * discr) / cl;
        }
        double v = 0.5 * cy - u;
        double w = 2.  * cy;
        double e = 0.5 / cx;
        double f = 0.5 * cy + u;
        Intersections out { cnt, { Vec2d(-e * (v * w - cl), v),
                                   Vec2d(-e * (w * f - cl), f) } };
        if (xy_swapped) {
            std::swap(out.pts[0].x(), out.pts[0].y());
            std::swap(out.pts[1].x(), out.pts[1].y());
        }
        out.pts[0] += pt2.cast<double>();
        out.pts[1] += pt2.cast<double>();

        assert(std::abs((out.pts[0] - pt1.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[1] - pt1.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[0] - pt2.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[1] - pt2.cast<double>()).norm() - d) < SCALED_EPSILON);
        return out;
    }

    // Return maximum two points, that are at distance "d" from both the line and point.
    Intersections line_point_equal_distance_points(const Line &line, const Point &ipt, const double d)
    {   
        assert(line.a != ipt && line.b != ipt);
        // Calculating two points of distance "d" to a ray and a point.
        // Point.
        Vec2d  pt   = ipt.cast<double>();
        Vec2d  lv   = (line.b - line.a).cast<double>();
        double l2   = lv.squaredNorm();
        Vec2d  lpv  = (line.a - ipt).cast<double>();
        double c    = cross2(lpv, lv);
        if (c < 0) {
            lv = - lv;
            c  = - c;
        }

        // Line equation (ax + by + c - d * sqrt(l2)).
        auto   a    = - lv.y();
        auto   b    = lv.x();
        // Line point shifted by -ipt is on the line.
        assert(std::abs(lpv.x() * a + lpv.y() * b + c) < SCALED_EPSILON);
        // Line vector (a, b) points towards ipt.
        assert(a * lpv.x() + b * lpv.y() < - SCALED_EPSILON);

#ifndef NDEBUG
        {
            // Foot point of ipt on line.
            Vec2d ft = Geometry::foot_pt(line, ipt);
            // Center point between ipt and line, its distance to both line and ipt is equal.
            Vec2d centerpt = 0.5 * (ft + pt) - pt;
            double dcenter = 0.5 * (ft - pt).norm();
            // Verify that the center point
            assert(std::abs(centerpt.x() * a + centerpt.y() * b + c - dcenter * sqrt(l2)) < SCALED_EPSILON * sqrt(l2));
        }
#endif // NDEBUG

        // Calculate the two intersection points.
        // With the help of Python package sympy:
        //      res = solve([a * x + b * y + c - d * sqrt(a**2 + b**2), x**2 + y**2 - d**2], [x, y])
        //      ccode(cse((res[0][0], res[0][1], res[1][0], res[1][1])))
        // where (a, b, c, d) is the line equation, not normalized (vector a,b is not normalized),
        // d is distance from the line and the point (0, 0).
        // The result is then shifted to ipt.

        double dscaled = d * sqrt(l2);
        double s       = c * (2. * dscaled - c);
        if (s < 0.)
            // Distance of pt from line is bigger than 2 * d.
            return Intersections { 0 };
        double u;
        int    cnt;
        // Avoid division by zero if a gets too small.
        bool   xy_swapped = std::abs(a) < std::abs(b);
        if (xy_swapped)
            std::swap(a, b);
        if (s == 0.) {
            // Distance of pt from line is 2 * d.
            cnt = 1;
            u   = 0.;
        } else {
            // Distance of pt from line is smaller than 2 * d.
            cnt = 2;
            u   = a * sqrt(s) / l2;
        }
        double e = dscaled - c;
        double f = b * e / l2;
        double g = f - u;
        double h = f + u;
        Intersections out { cnt, { Vec2d((- b * g + e) / a, g),
                                   Vec2d((- b * h + e) / a, h) } };
        if (xy_swapped) {
            std::swap(out.pts[0].x(), out.pts[0].y());
            std::swap(out.pts[1].x(), out.pts[1].y());
        }
        out.pts[0] += pt;
        out.pts[1] += pt;

        assert(std::abs(Geometry::ray_point_distance<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), out.pts[0]) - d) < SCALED_EPSILON);
        assert(std::abs(Geometry::ray_point_distance<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), out.pts[1]) - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[0] - ipt.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[1] - ipt.cast<double>()).norm() - d) < SCALED_EPSILON);
        return out;
    }

} // namespace detail

#ifndef NDEBUG
namespace debug
{
    // Verify that twin halfedges are stored next to the other in vd.
    bool verify_twin_halfedges_successive(const VD &vd, const Lines &lines)
    {
        for (size_t i = 0; i < vd.num_edges(); i += 2) {
            const VD::edge_type &e  = vd.edges()[i];
            const VD::edge_type &e2 = vd.edges()[i + 1];
            assert(e.twin() == &e2);
            assert(e2.twin() == &e);
            assert(e.is_secondary() == e2.is_secondary());
            if (e.is_secondary()) {
                assert(e.cell()->contains_point() != e2.cell()->contains_point());
                const VD::edge_type &ex = (e.cell()->contains_point() ? e : e2);
                // Verify that the Point defining the cell left of ex is an end point of a segment
                // defining the cell right of ex.
                const Line  &line0 = lines[ex.cell()->source_index()];
                const Line  &line1 = lines[ex.twin()->cell()->source_index()];
                const Point &pt    = (ex.cell()->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
                assert(pt == line1.a || pt == line1.b);
            }
        }
        return true;
    }

    bool verify_inside_outside_annotations(const VD &vd)
    {
        // Verify that "Colors" are set at all Voronoi entities.
        for (const VD::vertex_type &v : vd.vertices())
            assert(vertex_category(v) != VertexCategory::Unknown);
        for (const VD::edge_type &e : vd.edges())
            assert(edge_category(e) != EdgeCategory::Unknown);
        for (const VD::cell_type &c : vd.cells())
            assert(cell_category(c) != CellCategory::Unknown);

        // Verify consistency between markings of Voronoi cells, edges and verticies.
        for (const VD::cell_type &cell : vd.cells()) {
            const VD::edge_type *first_edge = cell.incident_edge();
            const VD::edge_type *edge       = first_edge;
            CellCategory         cc         = cell_category(cell);
            size_t               num_vertices_on_contour    = 0;
            size_t               num_vertices_inside        = 0;
            size_t               num_vertices_outside       = 0;
            size_t               num_edges_point_to_contour = 0;
            size_t               num_edges_point_inside     = 0;
            size_t               num_edges_point_outside    = 0;
            do {
                {
                    EdgeCategory ec = edge_category(edge);
                    switch (ec) {
                    case EdgeCategory::PointsInside:
                        ++ num_edges_point_inside; break;
                    case EdgeCategory::PointsOutside:
                        ++ num_edges_point_outside; break;
                    case EdgeCategory::PointsToContour:
                        ++ num_edges_point_to_contour; break;
                    default:
                        assert(false);
                    }
                }
                {
                    VertexCategory vc = (edge->vertex1() == nullptr) ? VertexCategory::Outside : vertex_category(edge->vertex1());
                    switch (vc) {
                    case VertexCategory::Inside:
                        ++ num_vertices_inside; break;
                    case VertexCategory::Outside:
                        ++ num_vertices_outside; break;
                    case VertexCategory::OnContour:
                        ++ num_vertices_on_contour; break;
                    default:
                        assert(false);
                    }
                }
                {
                    const VD::cell_type *cell_other = edge->twin()->cell();
                    const CellCategory   cc_other   = cell_category(cell_other);
                    assert(cc_other != CellCategory::Unknown);
                    switch (cc) {
                    case CellCategory::Boundary:
                        assert(cc_other != CellCategory::Boundary || cell_other->contains_segment());
                        assert(cell.contains_segment());
                        assert(num_edges_point_to_contour == 2);
                        assert(num_vertices_on_contour == 2);
                        assert(num_vertices_inside > 0);
                        assert(num_vertices_outside > 0);
                        assert(num_edges_point_inside > 0);
                        assert(num_edges_point_outside > 0);
                        break;
                    case CellCategory::Inside:
                        assert(one_of(cc_other, { CellCategory::Inside, CellCategory::Boundary }));
                        assert(num_vertices_on_contour <= 1);
                        assert(num_edges_point_to_contour <= 1);
                        assert(num_vertices_inside > 0);
                        assert(num_vertices_outside == 0);
                        assert(num_edges_point_inside > 0);
                        assert(num_edges_point_outside == 0);
                        break;
                    case CellCategory::Outside:
                        assert(one_of(cc_other, { CellCategory::Outside, CellCategory::Boundary }));
                        assert(num_vertices_on_contour <= 1);
                        assert(num_edges_point_to_contour <= 1);
                        assert(num_vertices_inside == 0);
                        assert(num_vertices_outside > 0);
                        assert(num_edges_point_inside == 0);
                        assert(num_edges_point_outside > 0);
                        break;
                    default:
                        assert(false);
                        break;
                    }
                }
                edge = edge->next();
            } while (edge != first_edge);

            switch (cc) {
            case CellCategory::Boundary:
                assert(cell.contains_segment());
                assert(num_edges_point_to_contour == 2);
                assert(num_vertices_on_contour == 2);
                assert(num_vertices_inside > 0);
                assert(num_vertices_outside > 0);
                assert(num_edges_point_inside > 0);
                assert(num_edges_point_outside > 0);
                break;
            case CellCategory::Inside:
                assert(num_vertices_on_contour <= 1);
                assert(num_edges_point_to_contour <= 1);
                assert(num_vertices_inside > 0);
                assert(num_vertices_outside == 0);
                assert(num_edges_point_inside > 0);
                assert(num_edges_point_outside == 0);
                break;
            case CellCategory::Outside:
                assert(num_vertices_on_contour <= 1);
                assert(num_edges_point_to_contour <= 1);
                assert(num_vertices_inside == 0);
                assert(num_vertices_outside > 0);
                assert(num_edges_point_inside == 0);
                assert(num_edges_point_outside > 0);
                break;
            default:
                assert(false);
                break;
            }
        }

        return true;
    }
}
#endif // NDEBUG

void reset_inside_outside_annotations(VD &vd)
{
    for (const VD::vertex_type &v : vd.vertices())
        set_vertex_category(const_cast<VD::vertex_type&>(v), VertexCategory::Unknown);
    for (const VD::edge_type &e : vd.edges())
        set_edge_category(const_cast<VD::edge_type&>(e), EdgeCategory::Unknown);
    for (const VD::cell_type &c : vd.cells())
        set_cell_category(const_cast<VD::cell_type&>(c), CellCategory::Unknown);
}

void annotate_inside_outside(VD &vd, const Lines &lines)
{
    assert(debug::verify_twin_halfedges_successive(vd, lines));

    reset_inside_outside_annotations(vd);

    // Set a VertexCategory, verify validity of the operation.
    auto annotate_vertex = [](const VD::vertex_type *vertex, VertexCategory new_vertex_category) {
        VertexCategory vc = vertex_category(vertex);
        assert(vc == VertexCategory::Unknown || vc == new_vertex_category);
        assert(new_vertex_category == VertexCategory::Inside || 
               new_vertex_category == VertexCategory::Outside ||
               new_vertex_category == VertexCategory::OnContour);
        set_vertex_category(const_cast<VD::vertex_type*>(vertex), new_vertex_category);
    };

    // Set an EdgeCategory, verify validity of the operation.
    auto annotate_edge = [](const VD::edge_type *edge, EdgeCategory new_edge_category) {
        EdgeCategory ec = edge_category(edge);
        assert(ec == EdgeCategory::Unknown || ec == new_edge_category);
        assert(new_edge_category == EdgeCategory::PointsInside || 
               new_edge_category == EdgeCategory::PointsOutside ||
               new_edge_category == EdgeCategory::PointsToContour);
        set_edge_category(const_cast<VD::edge_type*>(edge), new_edge_category);
    };

    // Set a CellCategory, verify validity of the operation.
    // Handle marking of boundary cells (first time the cell is marked as outside, the other time as inside).
    // Returns true if the current cell category was modified.
    auto annotate_cell = [](const VD::cell_type *cell, CellCategory new_cell_category) -> bool {
        CellCategory cc = cell_category(cell);
        assert(one_of(cc, { CellCategory::Inside, CellCategory::Outside, CellCategory::Boundary, CellCategory::Unknown }));
        assert(one_of(new_cell_category, { CellCategory::Inside, CellCategory::Outside, CellCategory::Boundary }));
        switch (cc) {
        case CellCategory::Unknown:
            // Old category unknown, just write the new category.
            break;
        case CellCategory::Outside:
            if (new_cell_category == CellCategory::Inside)
                new_cell_category = CellCategory::Boundary;
            break;
        case CellCategory::Inside:
            if (new_cell_category == CellCategory::Outside)
                new_cell_category = CellCategory::Boundary;
            break;
        case CellCategory::Boundary:
            return false;
        }
        if (cc != new_cell_category) {
            set_cell_category(const_cast<VD::cell_type*>(cell), new_cell_category);
            return true;
        }
        return false;
    };

    for (const VD::edge_type &edge : vd.edges())
        if (edge.vertex1() == nullptr) {
            // Infinite Voronoi edge separating two Point sites or a Point site and a Segment site.
            // Infinite edge is always outside and it references at least one valid vertex.
            assert(edge.is_infinite());
            assert(edge.is_linear());
            assert(edge.vertex0() != nullptr);
            annotate_edge(&edge, EdgeCategory::PointsOutside);
            // Opposite edge of an infinite edge is certainly not active.
            annotate_edge(edge.twin(), EdgeCategory::PointsToContour);
            annotate_vertex(edge.vertex0(), VertexCategory::OnContour);
            // edge.vertex1() is null, it is implicitely outside.
            if (edge.is_secondary()) {
                // edge.vertex0() must lie on the source contour.
                const VD::cell_type *cell  = edge.cell();
                const VD::cell_type *cell2 = edge.twin()->cell();
                if (cell->contains_segment())
                    std::swap(cell, cell2);
                // State of a cell containing a boundary point is certainly outside.
                assert(cell->contains_point());
                annotate_cell(cell, CellCategory::Outside);
                // State of a cell containing a boundary edge is Boundary.
                assert(cell2->contains_segment());
                annotate_cell(cell2, CellCategory::Boundary);
            }
        } else if (edge.vertex0() != nullptr) {
            assert(edge.is_finite());
            const VD::cell_type *cell = edge.cell();
            const Line          *line = cell->contains_segment() ? &lines[cell->source_index()] : nullptr;
            if (line == nullptr) {
                cell = edge.twin()->cell();
                line = cell->contains_segment() ? &lines[cell->source_index()] : nullptr;
            }
            if (line) {
                const VD::vertex_type *v1    = edge.vertex1();
                const VD::cell_type   *cell2 = (cell == edge.cell()) ? edge.twin()->cell() : edge.cell();
                assert(v1 != nullptr);
                const Point *pt_on_contour = nullptr;
                if (cell == edge.cell() && edge.twin()->cell()->contains_segment()) {
                    // Constrained bisector of two segments.
                    assert(edge.is_linear());
                    // If the two segments share a point, then one end of the current Voronoi edge shares this point as well.
                    // A bisector may not necessarily connect to the source contour. Find pt_on_contour if it exists.
                    const Line &line2 = lines[cell2->source_index()];
                    if (line->a == line2.b)
                        pt_on_contour = &line->a;
                    else if (line->b == line2.a)
                        pt_on_contour = &line->b;
                } else if (edge.is_secondary()) {
                    assert(edge.is_linear());
                    // One end of the current Voronoi edge shares a point of a contour.
                    assert(edge.cell()->contains_point() != edge.twin()->cell()->contains_point());
                    const Line &line2 = lines[cell2->source_index()];
                    pt_on_contour = &contour_point(*cell2, line2);
                }
                if (pt_on_contour) {
                    // One end of the current Voronoi edge shares a point of a contour.
                    // Find out which one it is.
                    const VD::vertex_type *v0 = edge.vertex0();
                    bool v1_on_contour = false;
                    if (coord_t(v0->x() + 0.5) == pt_on_contour->x() && 
                        coord_t(v0->y() + 0.5) == pt_on_contour->y()) {
                        if (coord_t(v1->x() + 0.5) == pt_on_contour->x() && 
                            coord_t(v1->y() + 0.5) == pt_on_contour->y()) {
                            // This is really a degenerate case, we don't want this to happen.
                            assert(false);
                            // If it happens, play safe and try to detect the more probable point on contour.
                            Vec2d vec0(v0->x() - pt_on_contour->x(), v0->y() - pt_on_contour->y());
                            Vec2d vec1(v1->x() - pt_on_contour->x(), v1->y() - pt_on_contour->y());
                            if (vec0.squaredNorm() > vec1.squaredNorm())
                                v1_on_contour = true;
                        }
                    } else {
                        // v1 is on the contour.
                        assert(coord_t(v1->x() + 0.5) == pt_on_contour->x() && 
                               coord_t(v1->y() + 0.5) == pt_on_contour->y());
                        v1_on_contour = true;
                    }
                    if (v1_on_contour) {
                        // Skip secondary edge pointing to a contour point.
                        annotate_edge(&edge, EdgeCategory::PointsToContour);
                        annotate_vertex(v1, VertexCategory::OnContour);
                        continue;
                    }
                }
                // v0 is certainly not on the input polygons.
                // Is v1 inside or outside the input polygons?
                // The Voronoi vertex coordinate is in doubles, calculate orientation in doubles.
                Vec2d l0(line->a.cast<double>());
                Vec2d lv((line->b - line->a).cast<double>());
                double side = cross2(Vec2d(v1->x(), v1->y()) - l0, lv);
                // No Voronoi edge could connect two vertices of input polygons.
                assert(side != 0.);
                auto vc = side > 0. ? VertexCategory::Outside : VertexCategory::Inside;
                annotate_vertex(v1, vc);
                annotate_edge(&edge, vc == VertexCategory::Outside ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside);
                if (pt_on_contour) {
                    annotate_vertex(edge.vertex0(), VertexCategory::OnContour);
                    annotate_edge(edge.twin(), EdgeCategory::PointsToContour);
                } else {
                    // Otherwise v0 is not on the contour and its inside / outside state will be set later.
                }
                assert(cell->contains_segment());
                annotate_cell(cell, pt_on_contour ? CellCategory::Boundary :
                    (vc == VertexCategory::Outside ? CellCategory::Outside : CellCategory::Inside));
                annotate_cell(cell2, (pt_on_contour && cell2->contains_segment()) ? CellCategory::Boundary :
                    (vc == VertexCategory::Outside ? CellCategory::Outside : CellCategory::Inside));
            }
        }

    // Now most Voronoi vertices, edges and cells are annotated, with the exception of some
    // edges separating two Point sites, their cells and vertices.
    // Perform one round of expansion marking Voronoi edges and cells next to boundary cells.
    std::vector<const VD::cell_type*> cell_queue;
    for (const VD::edge_type &edge : vd.edges()) {
        assert((edge_category(edge) == EdgeCategory::Unknown) == (edge_category(edge.twin()) == EdgeCategory::Unknown));
        if (edge_category(edge) == EdgeCategory::Unknown) {
            assert(edge.is_finite());
            const VD::cell_type &cell  = *edge.cell();
            const VD::cell_type &cell2 = *edge.twin()->cell();
            assert(cell.contains_point() && cell2.contains_point());
            VertexCategory vc    = vertex_category(edge.vertex0());
            if (vc != VertexCategory::Unknown) {
                assert(vc != VertexCategory::OnContour);
                // Propagate.
                annotate_vertex(edge.vertex1(), vc);
                auto ec_new = (vc == VertexCategory::Outside) ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside;
                annotate_edge(&edge, ec_new);
                annotate_edge(edge.twin(), ec_new);
                // Edge separating two point sources, not yet classified as inside / outside.
                CellCategory cc  = cell_category(cell);
                CellCategory cc2 = cell_category(cell2);
                auto cc_new = (vc == VertexCategory::Outside) ? CellCategory::Outside : CellCategory::Inside;
                assert(cc != CellCategory::Boundary && cc2 != CellCategory::Boundary);
                assert(cc == CellCategory::Unknown || cc == cc_new);
                if (cc != cc_new) {
                    annotate_cell(&cell, cc_new);
                    cell_queue.emplace_back(&cell);
                }
                if (cc2 != cc_new) {
                    annotate_cell(&cell2, cc_new);
                    cell_queue.emplace_back(&cell2);
                }
            }
        }
    }

    // Do a final seed fill over Voronoi cells and unmarked Voronoi edges.
    while (! cell_queue.empty()) {
        const VD::cell_type *cell = cell_queue.back();
        const CellCategory   cc   = cell_category(cell);
        assert(one_of(cc, { CellCategory::Outside, CellCategory::Inside }));
        cell_queue.pop_back();
        const VD::edge_type *first_edge = cell->incident_edge();
        const VD::edge_type *edge       = first_edge;
        const auto           ec_new     = (cc == CellCategory::Outside) ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside;
        do {
            EdgeCategory ec = edge_category(edge);
            if (ec == EdgeCategory::Unknown) {
                assert(edge->cell()->contains_point() && edge->twin()->cell()->contains_point());
                annotate_edge(edge, ec_new);
                annotate_edge(edge->twin(), ec_new);
                const VD::cell_type *cell2 = edge->twin()->cell();
                CellCategory cc2 = cell_category(cell2);
                assert(cc2 == CellCategory::Unknown || cc2 == cc);
                if (cc2 != cc) {
                    annotate_cell(cell2, cc);
                    cell_queue.emplace_back(cell2);
                }
            } else {
                assert(edge->vertex0() == nullptr || vertex_category(edge->vertex0()) != VertexCategory::Unknown);
                assert(edge->vertex1() == nullptr || vertex_category(edge->vertex1()) != VertexCategory::Unknown);
                assert(edge_category(edge->twin()) != EdgeCategory::Unknown);
                assert(cell_category(edge->cell()) != CellCategory::Unknown);
                assert(cell_category(edge->twin()->cell()) != CellCategory::Unknown);
            }
            edge = edge->next();
        } while (edge != first_edge);
    }

    assert(debug::verify_inside_outside_annotations(vd));
}

std::vector<double> signed_vertex_distances(const VD &vd, const Lines &lines)
{
    // vd shall be annotated.
    assert(debug::verify_inside_outside_annotations(vd));

    std::vector<double> out(vd.vertices().size(), 0.);
    const VD::vertex_type *first_vertex = &vd.vertices().front();
    for (const VD::vertex_type &vertex : vd.vertices()) {
        const VertexCategory vc = vertex_category(vertex);
        double dist;
        if (vc == VertexCategory::OnContour) {
            dist = 0.;
        } else {
            const VD::edge_type *first_edge = vertex.incident_edge();
            const VD::edge_type *edge       = first_edge;
            const VD::cell_type *point_cell = nullptr;
            do {
                if (edge->cell()->contains_point()) {
                    point_cell = edge->cell();
                    break;
                }
                edge = edge->rot_next();
            } while (edge != first_edge);
            if (point_cell == 0) {
                // Project vertex onto a contour segment.
                const Line &line = lines[edge->cell()->source_index()];
                dist = Geometry::ray_point_distance<Vec2d>(
                    line.a.cast<double>(), (line.b - line.a).cast<double>(), vertex_point(vertex));
            } else {
                // Distance to a contour point.
                dist = (contour_point(*point_cell, lines).cast<double>() - vertex_point(vertex)).norm();
            }
            if (vc == VertexCategory::Inside)
                dist = - dist;
        }
        out[&vertex - first_vertex] = dist;
    }

    return out;
}

std::vector<Vec2d> edge_offset_contour_intersections(
    const VD                    &vd,
    const Lines                 &lines,
    const std::vector<double>   &vertex_distances,
    double                       offset_distance)
{
    // vd shall be annotated.
    assert(debug::verify_inside_outside_annotations(vd));

    bool         outside          = offset_distance > 0;
    const double offset_distance2 = offset_distance * offset_distance;
    if (! outside)
        offset_distance = - offset_distance;
    assert(offset_distance > 0.);

    const VD::vertex_type *first_vertex = &vd.vertices().front();
    const VD::edge_type   *first_edge   = &vd.edges().front();
    static constexpr double nan         = std::numeric_limits<double>::quiet_NaN();
    // By default none edge has an intersection with the offset curve.
    std::vector<Vec2d>     out(vd.num_edges(), Vec2d(nan, 0.));

    for (const VD::edge_type &edge : vd.edges()) {
        size_t                 edge_idx = &edge - first_edge;
        if (edge_offset_has_intersection(out[edge_idx]) || out[edge_idx].y() != 0.)
            // This edge was already classified.
            continue;

        const VD::vertex_type *v0 = edge.vertex0();
        const VD::vertex_type *v1 = edge.vertex1();
        double d0 = (v0 == nullptr) ? std::numeric_limits<double>::max() : vertex_distances[v0 - first_vertex];
        double d1 = (v1 == nullptr) ? std::numeric_limits<double>::max() : vertex_distances[v1 - first_vertex];
        if (d0 == d1)
            continue;
        if (! outside) {
            d0 = - d0;
            d1 = - d1;
        }
        double dmin, dmax;
        if (d0 < d1)
            dmin = d0, dmax = d1;
        else
            dmax = d0, dmin = d1;
        // Offset distance may be lower than dmin, but never higher than dmax.
        // Don't intersect an edge at dmax
        //      1) To avoid zero edge length, zero area offset contours.
        //      2) To ensure that the offset contours that cross a Voronoi vertex are traced consistently
        //         at one side of the offset curve only.
        if (offset_distance >= dmax)
            continue;

        // Edge candidate, intersection points were not calculated yet.
        assert(v0 != nullptr);
        const VD::cell_type   *cell  = edge.cell();
        const VD::cell_type   *cell2 = edge.twin()->cell();
        const Line            &line0 = lines[cell->source_index()];
        const Line            &line1 = lines[cell2->source_index()];
        size_t                 edge_idx2 = edge.twin() - first_edge;
        if (v1 == nullptr) {
            assert(edge.is_infinite());
            assert(edge.is_linear());
            // Unconstrained edges have always montonous distance.
            if (offset_distance >= dmin) {
                // There is certainly an intersection with the offset curve.
                if (cell->contains_point() && cell2->contains_point()) {
                    assert(! edge.is_secondary());
                    const Point &pt0 = contour_point(*cell, line0);
                    const Point &pt1 = contour_point(*cell2, line1);
                    Vec2d dir(double(pt0.y() - pt1.y()), double(pt1.x() - pt0.x()));
                    Vec2d pt(v0->x(), v0->y());
                    double t = detail::first_circle_segment_intersection_parameter(Vec2d(pt0.x(), pt0.y()), offset_distance, pt, dir);
                    assert(t > 0.);
                    out[edge_idx] = pt + t * dir;
                } else {
                    // Infinite edges could not be created by two segment sites.
                    assert(cell->contains_point() != cell2->contains_point());
                    // Linear edge goes through the endpoint of a segment.
                    assert(edge.is_secondary());
                    const Point &ipt = cell->contains_segment() ? contour_point(*cell2, line1) : contour_point(*cell, line0);
    #ifndef NDEBUG
                    if (cell->contains_segment()) {
                        const Point &pt1 = contour_point(*cell2, line1);
                        assert(pt1 == line0.a || pt1 == line0.b);
                    } else {
                        const Point &pt0 = contour_point(*cell, line0);
                        assert(pt0 == line1.a || pt0 == line1.b);
                    }
                    assert((vertex_point(v0) - ipt.cast<double>()).norm() < SCALED_EPSILON);
    #endif /* NDEBUG */
                    // Infinite edge starts at an input contour, therefore there is always an intersection with an offset curve.
                    const Line &line = cell->contains_segment() ? line0 : line1;
                    assert(line.a == ipt || line.b == ipt);
                    out[edge_idx] = ipt.cast<double>() + offset_distance * Vec2d(line.b.y() - line.a.y(), line.a.x() - line.b.x()).normalized();
                }
            }
            // The other edge of an unconstrained edge starting with null vertex shall never be intersected. Mark it as visited.
            out[edge_idx2].y() = 1.;
        } else {
            assert(edge.is_finite());
            bool done = false;
            // Bisector of two line segments, distance along the bisector is linear.
            bool bisector = cell->contains_segment() && cell2->contains_segment();
            assert(edge.is_finite());
            // or a secondary line, again the distance along the secondary line is linear and starts at the contour (zero distance).
            if (bisector || edge.is_secondary()) {
                assert(edge.is_linear());
#ifndef NDEBUG
                if (edge.is_secondary()) {
                    assert(cell->contains_point() != cell2->contains_point());
                    // One of the vertices is on the input contour.
                    assert((vertex_category(edge.vertex0()) == VertexCategory::OnContour) !=
                           (vertex_category(edge.vertex1()) == VertexCategory::OnContour));
                    assert(dmin == 0.);
                }
#endif // NDEBUG
                if (! bisector || (dmin != dmax && offset_distance >= dmin)) {
                    double t = (offset_distance - dmin) / (dmax - dmin);
                    t = clamp(0., 1., t);
                    if (d1 < d0) {
                        out[edge_idx2] = Slic3r::lerp(vertex_point(v1), vertex_point(v0), t);
                        // mark visited
                        out[edge_idx].y() = 1.;
                    } else {
                        out[edge_idx] = Slic3r::lerp(vertex_point(v0), vertex_point(v1), t);
                        // mark visited
                        out[edge_idx2].y() = 1.;
                    }
                    done = true;
                }
            } else {
                // Point - Segment or Point - Point edge, distance along this Voronoi edge may not be monotonous,
                // there may be a minimum distance point somewhere along this Voronoi edge.
                assert(cell->contains_point() || cell2->contains_point());
                bool point_vs_segment = cell->contains_point() != cell2->contains_point();
                bool has_intersection = false;
                bool possibly_two_points = false;
                const Point &pt0 = cell->contains_point() ? contour_point(*cell, line0) : contour_point(*cell2, line1);
                // Project p0 to line segment <v0, v1>.
                Vec2d p0(v0->x(), v0->y());
                Vec2d p1(v1->x(), v1->y());
                Vec2d px(pt0.x(), pt0.y());
                if (offset_distance2 >= dmin) {
                    has_intersection = true;
                } else {
                    double dmin_new = dmin;
                    if (point_vs_segment) {
                        // Project on the source segment.
                        const Line &line    = cell->contains_segment() ? line0 : line1;
                        const Vec2d pt_line = line.a.cast<double>();
                        const Vec2d v_line  = (line.b - line.a).cast<double>();
                        double      t0      = (p0 - pt_line).dot(v_line);
                        double      t1      = (p1 - pt_line).dot(v_line);
                        double      tx      = (px - pt_line).dot(v_line);
                        if ((tx >= t0 && tx <= t1) || (tx >= t1 && tx <= t0)) {
                            // Projection of the Point site falls between the projections of the Voronoi edge end points
                            // onto the Line site.
                            Vec2d ft = pt_line + (tx / v_line.squaredNorm()) * v_line;
                            dmin_new = (ft - px).squaredNorm() * 0.25;
                        }
                    } else {
                        // Point-Point Voronoi sites. Project point site onto the current Voronoi edge.
                        Vec2d  v   = p1 - p0;
                        auto   l2  = v.squaredNorm();
                        assert(l2 > 0);
                        auto   t   = v.dot(px - p0);
                        if (t >= 0. && t <= l2) {
                            // Projection falls onto the Voronoi edge. Calculate foot point and distance.
                            Vec2d ft = p0 + (t / l2) * v;
                            dmin_new = (ft - px).squaredNorm();
                        }
                    }
                    assert(dmin_new < dmax + SCALED_EPSILON);
                    assert(dmin_new < dmin + SCALED_EPSILON);
                    if (dmin_new < dmin) {
                        dmin = dmin_new;
                        has_intersection = possibly_two_points = offset_distance2 >= dmin;
                    }
                }
                if (has_intersection) {
                    detail::Intersections intersections;
                    if (point_vs_segment) {
                        assert(cell->contains_point() || cell2->contains_point());
                        intersections = detail::line_point_equal_distance_points(cell->contains_segment() ? line0 : line1, pt0, offset_distance);
                    } else {
                        const Point &pt1 = contour_point(*cell2, line1);
                        intersections = detail::point_point_equal_distance_points(pt0, pt1, offset_distance);
                    }
                    // If the span of distances of start / end point / foot point to the point site indicate an intersection,
                    // we should find one.
                    assert(intersections.count > 0);
                    if (intersections.count == 2) {
                        // Now decide which points fall on this Voronoi edge.
                        // Tangential points (single intersection) are ignored.
                        if (possibly_two_points) {
                            Vec2d  v  = p1 - p0;
                            double l2 = v.squaredNorm();
                            double t0 = v.dot(intersections.pts[0] - p0);
                            double t1 = v.dot(intersections.pts[1] - p0);
                            if (t0 > t1) {
                                std::swap(t0, t1);
                                std::swap(intersections.pts[0], intersections.pts[1]);
                            }
                            // Remove points outside of the line range.
                            if (t0 < 0. || t0 > l2) {
                                if (t1 < 0. || t1 > l2)
                                    intersections.count = 0;
                                else {
                                    -- intersections.count;
                                    t0 = t1;
                                    intersections.pts[0] = intersections.pts[1];
                                }
                            } else if (t1 < 0. || t1 > l2)
                                -- intersections.count;
                        } else {
                            // Take the point furthest from the end points of the Voronoi edge or a Voronoi parabolic arc.
                            double d0 = std::max((intersections.pts[0] - p0).squaredNorm(), (intersections.pts[0] - p1).squaredNorm());
                            double d1 = std::max((intersections.pts[1] - p0).squaredNorm(), (intersections.pts[1] - p1).squaredNorm());
                            if (d0 > d1)
                                intersections.pts[0] = intersections.pts[1];
                            -- intersections.count;
                        }
                        assert(intersections.count > 0);
                        if (intersections.count == 2) {
                            out[edge_idx] = intersections.pts[1];
                            out[edge_idx2] = intersections.pts[0];
                            done = true;
                        } else if (intersections.count == 1) {
                            if (d1 < d0)
                                std::swap(edge_idx, edge_idx2);
                            out[edge_idx] = intersections.pts[0];
                            out[edge_idx2].y() = 1.;
                            done = true;
                        }
                    }
                }
            }
            if (! done)
                out[edge_idx].y() = out[edge_idx2].y() = 1.;
        }
    }

    return out;
}

Polygons offset(
    const Geometry::VoronoiDiagram  &vd,
    const Lines                     &lines,
    const std::vector<double>       &signed_vertex_distances,
    double                           offset_distance,
    double                           discretization_error)
{
    std::vector<Vec2d> edge_points = edge_offset_contour_intersections(vd, lines, signed_vertex_distances, offset_distance);

    const VD::edge_type *front_edge = &vd.edges().front();

    auto next_offset_edge = [&edge_points, front_edge](const VD::edge_type *start_edge) -> const VD::edge_type* {
	    for (const VD::edge_type *edge = start_edge->next(); edge != start_edge; edge = edge->next())
            if (edge_offset_has_intersection(edge_points[edge->twin() - front_edge]))
                return edge->twin();
        // assert(false);
        return nullptr;
	};

#ifndef NDEBUG
	auto dist_to_site = [&lines](const VD::cell_type &cell, const Vec2d &point) {
        const Line &line = lines[cell.source_index()];
        return cell.contains_point() ?
            (((cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line.a : line.b).cast<double>() - point).norm() :
            (Geometry::foot_pt<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), point) - point).norm();
	};
#endif /* NDEBUG */

	// Track the offset curves.
	Polygons out;
	double angle_step    = 2. * acos((offset_distance - discretization_error) / offset_distance);
    double cos_threshold = cos(angle_step);
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();
	for (size_t seed_edge_idx = 0; seed_edge_idx < vd.num_edges(); ++ seed_edge_idx) {
        Vec2d last_pt = edge_points[seed_edge_idx];
        if (edge_offset_has_intersection(last_pt)) {
            const VD::edge_type *start_edge = &vd.edges()[seed_edge_idx];
            const VD::edge_type *edge       = start_edge;
            Polygon  			 poly;
		    do {
		        // find the next edge
                const VD::edge_type *next_edge = next_offset_edge(edge);
#ifdef VORONOI_DEBUG_OUT
                if (next_edge == nullptr) {
                    Lines helper_lines;
                    dump_voronoi_to_svg(debug_out_path("voronoi-offset-open-loop-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), to_lines(poly));
                }
#endif // VORONOI_DEBUG_OUT
                assert(next_edge);
		        //std::cout << "offset-output: "; print_edge(edge); std::cout << " to "; print_edge(next_edge); std::cout << "\n";
		        // Interpolate a circular segment or insert a linear segment between edge and next_edge.
                const VD::cell_type  *cell      = edge->cell();
                // Mark the edge / offset curve intersection point as consumed.
                Vec2d p1 = last_pt;
                Vec2d p2 = edge_points[next_edge - front_edge];
                edge_points[next_edge - front_edge].x() = nan;
#ifndef NDEBUG
                {
                    double err  = dist_to_site(*cell, p1) - offset_distance;
                    double err2 = dist_to_site(*cell, p2) - offset_distance;
#ifdef VORONOI_DEBUG_OUT
                    if (std::max(err, err2) >= SCALED_EPSILON) {
                        Lines helper_lines;
                        dump_voronoi_to_svg(debug_out_path("voronoi-offset-incorrect_pt-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), to_lines(poly));
                    }
#endif // VORONOI_DEBUG_OUT
                    assert(std::abs(err) < SCALED_EPSILON);
                    assert(std::abs(err2) < SCALED_EPSILON);
                }
#endif /* NDEBUG */
				if (cell->contains_point()) {
					// Discretize an arc from p1 to p2 with radius = offset_distance and discretization_error.
                    // The extracted contour is CCW oriented, extracted holes are CW oriented.
                    // The extracted arc will have the same orientation. As the Voronoi regions are convex, the angle covered by the arc will be convex as well.
                    const Line  &line0  = lines[cell->source_index()];
					const Vec2d &center = ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b).cast<double>();
					const Vec2d  v1 	= p1 - center;
					const Vec2d  v2 	= p2 - center;
                    bool 		 ccw    = cross2(v1, v2) > 0;
                    double       cos_a  = v1.dot(v2);
                    double       norm   = v1.norm() * v2.norm();
                    assert(norm > 0.);
                    if (cos_a < cos_threshold * norm) {
						// Angle is bigger than the threshold, therefore the arc will be discretized.
                        cos_a /= norm;
                        assert(cos_a > -1. - EPSILON && cos_a < 1. + EPSILON);
                        double angle = acos(std::max(-1., std::min(1., cos_a)));
						size_t n_steps = size_t(ceil(angle / angle_step));
						double astep = angle / n_steps;
						if (! ccw)
							astep *= -1.;
						double a = astep;
						for (size_t i = 1; i < n_steps; ++ i, a += astep) {
							double c = cos(a);
							double s = sin(a);
							Vec2d  p = center + Vec2d(c * v1.x() - s * v1.y(), s * v1.x() + c * v1.y());
                            poly.points.emplace_back(Point(coord_t(p.x()), coord_t(p.y())));
						}
                    }
				}
                {
                    Point pt_last(coord_t(p2.x()), coord_t(p2.y()));
                    if (poly.empty() || poly.points.back() != pt_last)
                        poly.points.emplace_back(pt_last);
                }
                edge = next_edge;
                last_pt = p2;
		    } while (edge != start_edge);
		    out.emplace_back(std::move(poly));
		}
    }

	return out;
}

#if 0
Polygons offset(
    const Geometry::VoronoiDiagram  &vd,
    const Lines                     &lines,
    double                           offset_distance,
    double                           discretization_error)
{
#ifndef NDEBUG
    // Verify that twin halfedges are stored next to the other in vd.
    for (size_t i = 0; i < vd.num_edges(); i += 2) {
        const VD::edge_type &e  = vd.edges()[i];
        const VD::edge_type &e2 = vd.edges()[i + 1];
        assert(e.twin() == &e2);
        assert(e2.twin() == &e);
        assert(e.is_secondary() == e2.is_secondary());
        if (e.is_secondary()) {
            assert(e.cell()->contains_point() != e2.cell()->contains_point());
            const VD::edge_type &ex = (e.cell()->contains_point() ? e : e2);
            // Verify that the Point defining the cell left of ex is an end point of a segment
            // defining the cell right of ex.
            const Line  &line0 = lines[ex.cell()->source_index()];
            const Line  &line1 = lines[ex.twin()->cell()->source_index()];
            const Point &pt    = (ex.cell()->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
            assert(pt == line1.a || pt == line1.b);
        }
    }
#endif // NDEBUG

    enum class EdgeState : unsigned char {
        // Initial state, don't know.
        Unknown,
        // This edge will certainly not be intersected by the offset curve.
        Inactive,
        // This edge will certainly be intersected by the offset curve.
        Active,
        // This edge will possibly be intersected by the offset curve.
        Possible
    };

    enum class CellState : unsigned char {
        // Initial state, don't know.
        Unknown,
        // Inactive cell is inside for outside curves and outside for inside curves.
        Inactive,
        // Active cell is outside for outside curves and inside for inside curves.
        Active,
        // Boundary cell is intersected by the input segment, part of it is active.
        Boundary
    };

    // Mark edges with outward vertex pointing outside the polygons, thus there is a chance
    // that such an edge will have an intersection with our desired offset curve.
    bool                    outside = offset_distance > 0.;
    std::vector<EdgeState>  edge_state(vd.num_edges(), EdgeState::Unknown);
    std::vector<CellState>  cell_state(vd.num_cells(), CellState::Unknown);
    const VD::edge_type    *front_edge = &vd.edges().front();
    const VD::cell_type    *front_cell = &vd.cells().front();
    auto                    set_edge_state_initial = [&edge_state, front_edge](const VD::edge_type *edge, EdgeState new_edge_type) {
        EdgeState &edge_type = edge_state[edge - front_edge];
        assert(edge_type == EdgeState::Unknown || edge_type == new_edge_type);
        assert(new_edge_type == EdgeState::Possible || new_edge_type == EdgeState::Inactive);
        edge_type = new_edge_type;
    };
    auto                    set_edge_state_final = [&edge_state, front_edge](const size_t edge_id, EdgeState new_edge_type) {
        EdgeState &edge_type = edge_state[edge_id];
        assert(edge_type == EdgeState::Possible || edge_type == new_edge_type);
        assert(new_edge_type == EdgeState::Active || new_edge_type == EdgeState::Inactive);
        edge_type = new_edge_type;
    };
    auto                    set_cell_state = [&cell_state, front_cell](const VD::cell_type *cell, CellState new_cell_type) -> bool {
        CellState &cell_type = cell_state[cell - front_cell];
        assert(cell_type == CellState::Active || cell_type == CellState::Inactive || cell_type == CellState::Boundary || cell_type == CellState::Unknown);
        assert(new_cell_type == CellState::Active || new_cell_type == CellState::Inactive || new_cell_type == CellState::Boundary);
        switch (cell_type) {
        case CellState::Unknown:
            break;
        case CellState::Active:
            if (new_cell_type == CellState::Inactive)
                new_cell_type = CellState::Boundary;
            break;
        case CellState::Inactive:
            if (new_cell_type == CellState::Active)
                new_cell_type = CellState::Boundary;
            break;
        case CellState::Boundary:
            return false;
        }
        if (cell_type != new_cell_type) {
            cell_type = new_cell_type;
            return true;
        }
        return false;
    };

    for (const VD::edge_type &edge : vd.edges())
        if (edge.vertex1() == nullptr) {
            // Infinite Voronoi edge separating two Point sites or a Point site and a Segment site.
            // Infinite edge is always outside and it has at least one valid vertex.
            assert(edge.vertex0() != nullptr);
            set_edge_state_initial(&edge, outside ? EdgeState::Possible : EdgeState::Inactive);
            // Opposite edge of an infinite edge is certainly not active.
            set_edge_state_initial(edge.twin(), EdgeState::Inactive);
            if (edge.is_secondary()) {
                // edge.vertex0() must lie on source contour.
                const VD::cell_type *cell  = edge.cell();
                const VD::cell_type *cell2 = edge.twin()->cell();
                if (cell->contains_segment())
                    std::swap(cell, cell2);
                // State of a cell containing a boundary point is known.
                assert(cell->contains_point());
                set_cell_state(cell, outside ? CellState::Active : CellState::Inactive);
                // State of a cell containing a boundary edge is Boundary.
                assert(cell2->contains_segment());
                set_cell_state(cell2, CellState::Boundary);
            }
        } else if (edge.vertex0() != nullptr) {
            // Finite edge.
            const VD::cell_type *cell = edge.cell();
            const Line          *line = cell->contains_segment() ? &lines[cell->source_index()] : nullptr;
            if (line == nullptr) {
                cell = edge.twin()->cell();
                line = cell->contains_segment() ? &lines[cell->source_index()] : nullptr;
            }
            if (line) {
                const VD::vertex_type *v1    = edge.vertex1();
                const VD::cell_type   *cell2 = (cell == edge.cell()) ? edge.twin()->cell() : edge.cell();
                assert(v1);
                const Point *pt_on_contour = nullptr;
                if (cell == edge.cell() && edge.twin()->cell()->contains_segment()) {
                    // Constrained bisector of two segments.
                    // If the two segments share a point, then one end of the current Voronoi edge shares this point as well.
                    // Find pt_on_contour if it exists.
                    const Line &line2 = lines[cell2->source_index()];
                    if (line->a == line2.b)
                        pt_on_contour = &line->a;
                    else if (line->b == line2.a)
                        pt_on_contour = &line->b;
                } else if (edge.is_secondary()) {
                    assert(edge.is_linear());
                    // One end of the current Voronoi edge shares a point of a contour.
                    assert(edge.cell()->contains_point() != edge.twin()->cell()->contains_point());
                    const Line &line2 = lines[cell2->source_index()];
                    pt_on_contour = &((cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line2.a : line2.b);
                }
                if (pt_on_contour) {
                    // One end of the current Voronoi edge shares a point of a contour.
                    // Find out which one it is.
                    const VD::vertex_type   *v0 = edge.vertex0();
                    Vec2d                    vec0(v0->x() - pt_on_contour->x(), v0->y() - pt_on_contour->y());
                    Vec2d                    vec1(v1->x() - pt_on_contour->x(), v1->y() - pt_on_contour->y());
                    double                   d0 = vec0.squaredNorm();
                    double                   d1 = vec1.squaredNorm();
                    assert(std::min(d0, d1) < SCALED_EPSILON * SCALED_EPSILON);
                    if (d0 < d1) {
                        // v0 is equal to pt.
                    } else {
                        // Skip secondary edge pointing to a contour point.
                        set_edge_state_initial(&edge, EdgeState::Inactive);
                        continue;
                    }
                }
                Vec2d l0(line->a.cast<double>());
                Vec2d lv((line->b - line->a).cast<double>());
                double side = cross2(lv, Vec2d(v1->x(), v1->y()) - l0);
                bool edge_active = outside ? (side < 0.) : (side > 0.);
                set_edge_state_initial(&edge, edge_active ? EdgeState::Possible : EdgeState::Inactive);
                assert(cell->contains_segment());
                set_cell_state(cell, 
                    pt_on_contour ? CellState::Boundary :
                                    edge_active ? CellState::Active : CellState::Inactive);
                set_cell_state(cell2,
                    (pt_on_contour && cell2->contains_segment()) ?
                        CellState::Boundary :
                        edge_active ? CellState::Active : CellState::Inactive);
            }
        }
    {
        // Perform one round of expansion marking Voronoi edges and cells next to boundary cells as active / inactive.
        std::vector<const VD::cell_type*> cell_queue;
        for (const VD::edge_type &edge : vd.edges())
            if (edge_state[&edge - front_edge] == EdgeState::Unknown) {
                assert(edge.cell()->contains_point() && edge.twin()->cell()->contains_point());
                // Edge separating two point sources, not yet classified as inside / outside.
                CellState cs  = cell_state[edge.cell() - front_cell];
                CellState cs2 = cell_state[edge.twin()->cell() - front_cell];
                if (cs != CellState::Unknown || cs2 != CellState::Unknown) {
                    if (cs == CellState::Unknown) {
                        cs = cs2;
                        if (set_cell_state(edge.cell(), cs))
                            cell_queue.emplace_back(edge.cell());
                    } else if (set_cell_state(edge.twin()->cell(), cs))
                        cell_queue.emplace_back(edge.twin()->cell());
                    EdgeState es = (cs == CellState::Active) ? EdgeState::Possible : EdgeState::Inactive;
                    set_edge_state_initial(&edge, es);
                    set_edge_state_initial(edge.twin(), es);
                } else {
                    const VD::edge_type *e = edge.twin()->rot_prev();
                    do {
                        EdgeState es = edge_state[e->twin() - front_edge];
                        if (es != EdgeState::Unknown) {
                            assert(es == EdgeState::Possible || es == EdgeState::Inactive);
                            set_edge_state_initial(&edge, es);
                            CellState cs = (es == EdgeState::Possible) ? CellState::Active : CellState::Inactive;
                            if (set_cell_state(edge.cell(), cs))
                                cell_queue.emplace_back(edge.cell());
                            if (set_cell_state(edge.twin()->cell(), cs))
                                cell_queue.emplace_back(edge.twin()->cell());
                            break;
                        }
                        e = e->rot_prev();
                    } while (e != edge.twin());
                }
            }
        // Do a final seed fill over Voronoi cells and unmarked Voronoi edges.
        while (! cell_queue.empty()) {
            const VD::cell_type *cell       = cell_queue.back();
            const CellState      cs         = cell_state[cell - front_cell];
            cell_queue.pop_back();
            const VD::edge_type *first_edge = cell->incident_edge();
            const VD::edge_type *edge       = cell->incident_edge();
            EdgeState            es         = (cs == CellState::Active) ? EdgeState::Possible : EdgeState::Inactive;
            do {
                if (set_cell_state(edge->twin()->cell(), cs)) {
                    set_edge_state_initial(edge, es);
                    set_edge_state_initial(edge->twin(), es);
                    cell_queue.emplace_back(edge->twin()->cell());
                }
                edge = edge->next();
            } while (edge != first_edge);
        }
    }

    if (! outside)
        offset_distance = - offset_distance;

#ifdef VORONOI_DEBUG_OUT
    BoundingBox bbox;
    {
        bbox.merge(get_extents(lines));
        bbox.min -= (0.01 * bbox.size().cast<double>()).cast<coord_t>();
        bbox.max += (0.01 * bbox.size().cast<double>()).cast<coord_t>();
    }
    static int irun = 0;
    ++ irun;
    {
        Lines helper_lines;
        for (const VD::edge_type &edge : vd.edges())
            if (edge_state[&edge - front_edge] == EdgeState::Possible) {
                const VD::vertex_type *v0 = edge.vertex0();
                const VD::vertex_type *v1 = edge.vertex1();
                assert(v0 != nullptr);
                Vec2d pt1(v0->x(), v0->y());
                Vec2d pt2;
                if (v1 == nullptr) {
                    // Unconstrained edge. Calculate a trimmed position.
                    assert(edge.is_linear());
                    const VD::cell_type *cell  = edge.cell();
                    const VD::cell_type *cell2 = edge.twin()->cell();
                    const Line          &line0 = lines[cell->source_index()];
                    const Line          &line1 = lines[cell2->source_index()];
                    if (cell->contains_point() && cell2->contains_point()) {
                        const Point &pt0 = (cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
                        const Point &pt1 = (cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b;
                        // Direction vector of this unconstrained Voronoi edge.
                        Vec2d dir(double(pt0.y() - pt1.y()), double(pt1.x() - pt0.x()));
                        pt2 = Vec2d(v0->x(), v0->y()) + dir.normalized() * scale_(10.);
                    } else {
                        // Infinite edges could not be created by two segment sites.
                        assert(cell->contains_point() != cell2->contains_point());
                        // Linear edge goes through the endpoint of a segment.
                        assert(edge.is_secondary());
                        const Point &ipt = cell->contains_segment() ?
                            ((cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b) :
                            ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b);
                        // Infinite edge starts at an input contour, therefore there is always an intersection with an offset curve.
                        const Line  &line = cell->contains_segment() ? line0 : line1;
                        assert(line.a == ipt || line.b == ipt);
                        // dir is perpendicular to line.
                        Vec2d dir(line.a.y() - line.b.y(), line.b.x() - line.a.x());
                        assert(dir.norm() > 0.);
                        if (((line.a == ipt) == cell->contains_point()) == (v0 == nullptr))
                            dir = - dir;
                        pt2 = ipt.cast<double>() + dir.normalized() * scale_(10.);
                    }
                } else {
                    pt2 = Vec2d(v1->x(), v1->y());
                    // Clip the line by the bounding box, so that the coloring of the line will be visible.
                    Geometry::liang_barsky_line_clipping(pt1, pt2, BoundingBoxf(bbox.min.cast<double>(), bbox.max.cast<double>()));
                }
                helper_lines.emplace_back(Line(Point(pt1.cast<coord_t>()), Point(((pt1 + pt2) * 0.5).cast<coord_t>())));
            }
        dump_voronoi_to_svg(debug_out_path("voronoi-offset-candidates1-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), helper_lines);
    }
#endif // VORONOI_DEBUG_OUT

    std::vector<Vec2d> edge_offset_point(vd.num_edges(), Vec2d());
    const double offset_distance2 = offset_distance * offset_distance;
    for (const VD::edge_type &edge : vd.edges()) {
        assert(edge_state[&edge - front_edge] != EdgeState::Unknown);
        size_t edge_idx = &edge - front_edge;
        if (edge_state[edge_idx] == EdgeState::Possible) {
            // Edge candidate, intersection points were not calculated yet.
            const VD::vertex_type *v0    = edge.vertex0();
            const VD::vertex_type *v1    = edge.vertex1();
            assert(v0 != nullptr);
            const VD::cell_type   *cell  = edge.cell();
            const VD::cell_type   *cell2 = edge.twin()->cell();
            const Line            &line0 = lines[cell->source_index()];
            const Line            &line1 = lines[cell2->source_index()];
            size_t                 edge_idx2 = edge.twin() - front_edge;
            if (v1 == nullptr) {
                assert(edge.is_infinite());
                assert(edge.is_linear());
                assert(edge_state[edge_idx2] == EdgeState::Inactive);
                if (cell->contains_point() && cell2->contains_point()) {
                    assert(! edge.is_secondary());
                    const Point &pt0 = (cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
                    const Point &pt1 = (cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b;
                    double dmin2 = (Vec2d(v0->x(), v0->y()) - pt0.cast<double>()).squaredNorm();
                    assert(dmin2 >= SCALED_EPSILON * SCALED_EPSILON);
                    if (dmin2 <= offset_distance2) {
                        // There shall be an intersection of this unconstrained edge with the offset curve.
                        // Direction vector of this unconstrained Voronoi edge.
                        Vec2d dir(double(pt0.y() - pt1.y()), double(pt1.x() - pt0.x()));
                        Vec2d pt(v0->x(), v0->y());
                        double t = detail::first_circle_segment_intersection_parameter(Vec2d(pt0.x(), pt0.y()), offset_distance, pt, dir);
                        edge_offset_point[edge_idx] = pt + t * dir;
                        set_edge_state_final(edge_idx, EdgeState::Active);
                    } else
                        set_edge_state_final(edge_idx, EdgeState::Inactive);
                } else {
                    // Infinite edges could not be created by two segment sites.
                    assert(cell->contains_point() != cell2->contains_point());
                    // Linear edge goes through the endpoint of a segment.
                    assert(edge.is_secondary());
                    const Point &ipt = cell->contains_segment() ?
                        ((cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b) :
                        ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b);
    #ifndef NDEBUG
                    if (cell->contains_segment()) {
                        const Point &pt1 = (cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b;
                        assert((pt1.x() == line0.a.x() && pt1.y() == line0.a.y()) ||
                               (pt1.x() == line0.b.x() && pt1.y() == line0.b.y()));
                    } else {
                        const Point &pt0 = (cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
                        assert((pt0.x() == line1.a.x() && pt0.y() == line1.a.y()) ||
                               (pt0.x() == line1.b.x() && pt0.y() == line1.b.y()));
                    }
                    assert((Vec2d(v0->x(), v0->y()) - ipt.cast<double>()).norm() < SCALED_EPSILON);
    #endif /* NDEBUG */
                    // Infinite edge starts at an input contour, therefore there is always an intersection with an offset curve.
                    const Line &line = cell->contains_segment() ? line0 : line1;
                    assert(line.a == ipt || line.b == ipt);
                    edge_offset_point[edge_idx] = ipt.cast<double>() + offset_distance * Vec2d(line.b.y() - line.a.y(), line.a.x() - line.b.x()).normalized();
                    set_edge_state_final(edge_idx, EdgeState::Active);
                }
                // The other edge of an unconstrained edge starting with null vertex shall never be intersected.
                set_edge_state_final(edge_idx2, EdgeState::Inactive);
            } else if (edge.is_secondary()) {
                assert(edge.is_linear());
                assert(cell->contains_point() != cell2->contains_point());
                const Line  &line0 = lines[edge.cell()->source_index()];
                const Line  &line1 = lines[edge.twin()->cell()->source_index()];
                const Point &pt    = cell->contains_point() ?
                    ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b) :
                    ((cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b);
                const Line  &line  = cell->contains_segment() ? line0 : line1;
                assert(pt == line.a || pt == line.b);
                assert((pt.cast<double>() - Vec2d(v0->x(), v0->y())).norm() < SCALED_EPSILON);
                Vec2d dir(v1->x() - v0->x(), v1->y() - v0->y());
                double l2 = dir.squaredNorm();
                if (offset_distance2 <= l2) {
                    edge_offset_point[edge_idx] = pt.cast<double>() + (offset_distance / sqrt(l2)) * dir;
                    set_edge_state_final(edge_idx, EdgeState::Active);
                } else {
                    set_edge_state_final(edge_idx, EdgeState::Inactive);
                }
                set_edge_state_final(edge_idx2, EdgeState::Inactive);
            } else {
                // Finite edge has valid points at both sides.
                bool done = false;
                if (cell->contains_segment() && cell2->contains_segment()) {
                    // This edge is a bisector of two line segments. Project v0, v1 onto one of the line segments.
                    Vec2d  pt(line0.a.cast<double>());
                    Vec2d  dir(line0.b.cast<double>() - pt);
                    Vec2d  vec0 = Vec2d(v0->x(), v0->y()) - pt;
                    Vec2d  vec1 = Vec2d(v1->x(), v1->y()) - pt;
                    double l2   = dir.squaredNorm();
                    assert(l2 > 0.);
                    double dmin = (dir * (vec0.dot(dir) / l2) - vec0).squaredNorm();
                    double dmax = (dir * (vec1.dot(dir) / l2) - vec1).squaredNorm();
                    bool   flip = dmin > dmax;
                    if (flip)
                        std::swap(dmin, dmax);
                    if (offset_distance2 >= dmin && offset_distance2 <= dmax) {
                        // Intersect. Maximum one intersection will be found.
                        // This edge is a bisector of two line segments. Distance to the input polygon increases/decreases monotonically.
                        dmin = sqrt(dmin);
                        dmax = sqrt(dmax);
                        assert(offset_distance > dmin - EPSILON && offset_distance < dmax + EPSILON);
                        double ddif = dmax - dmin;
                        if (ddif == 0.) {
                            // line, line2 are exactly parallel. This is a singular case, the offset curve should miss it.
                        } else {
                            if (flip) {
                                std::swap(edge_idx, edge_idx2);
                                std::swap(v0, v1);
                            }
                            double t = clamp(0., 1., (offset_distance - dmin) / ddif);
                            edge_offset_point[edge_idx] = Vec2d(lerp(v0->x(), v1->x(), t), lerp(v0->y(), v1->y(), t));
                            set_edge_state_final(edge_idx, EdgeState::Active);
                            set_edge_state_final(edge_idx2, EdgeState::Inactive);
                            done = true;
                        }
                    }
                } else {
                    assert(cell->contains_point() || cell2->contains_point());
                    bool point_vs_segment = cell->contains_point() != cell2->contains_point();
                    const Point &pt0 = cell->contains_point() ?
                        ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b) :
                        ((cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b);
                    // Project p0 to line segment <v0, v1>.
                    Vec2d p0(v0->x(), v0->y());
                    Vec2d p1(v1->x(), v1->y());
                    Vec2d px(pt0.x(), pt0.y());
                    double d0 = (p0 - px).squaredNorm();
                    double d1 = (p1 - px).squaredNorm();
                    double dmin = std::min(d0, d1);
                    double dmax = std::max(d0, d1);
                    bool has_intersection = false;
                    bool possibly_two_points = false;
                    if (offset_distance2 <= dmax) {
                        if (offset_distance2 >= dmin) {
                            has_intersection = true;
                        } else {
                            double dmin_new = dmin;
                            if (point_vs_segment) {
                                // Project on the source segment.
                                const Line &line    = cell->contains_segment() ? line0 : line1;
                                const Vec2d pt_line = line.a.cast<double>();
                                const Vec2d v_line  = (line.b - line.a).cast<double>();
                                double      t0      = (p0 - pt_line).dot(v_line);
                                double      t1      = (p1 - pt_line).dot(v_line);
                                double      tx      = (px - pt_line).dot(v_line);
                                if ((tx >= t0 && tx <= t1) || (tx >= t1 && tx <= t0)) {
                                    // Projection of the Point site falls between the projections of the Voronoi edge end points
                                    // onto the Line site.
                                    Vec2d ft = pt_line + (tx / v_line.squaredNorm()) * v_line;
                                    dmin_new = (ft - px).squaredNorm() * 0.25;
                                }
                            } else {
                                // Point-Point Voronoi sites. Project point site onto the current Voronoi edge.
                                Vec2d  v   = p1 - p0;
                                auto   l2  = v.squaredNorm();
                                assert(l2 > 0);
                                auto   t   = v.dot(px - p0);
                                if (t >= 0. && t <= l2) {
                                    // Projection falls onto the Voronoi edge. Calculate foot point and distance.
                                    Vec2d  ft = p0 + (t / l2) * v;
                                    dmin_new = (ft - px).squaredNorm();
                                }
                            }
                            assert(dmin_new < dmax + SCALED_EPSILON);
                            assert(dmin_new < dmin + SCALED_EPSILON);
                            if (dmin_new < dmin) {
                                dmin = dmin_new;
                                has_intersection = possibly_two_points = offset_distance2 >= dmin;
                            }
                        }
                    }
                    if (has_intersection) {
                        detail::Intersections intersections;
                        if (point_vs_segment) {
                            assert(cell->contains_point() || cell2->contains_point());
                            intersections = detail::line_point_equal_distance_points(cell->contains_segment() ? line0 : line1, pt0, offset_distance);
                        } else {
                            const Point &pt1 = (cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b;
                            intersections = detail::point_point_equal_distance_points(pt0, pt1, offset_distance);
                        }
                        // If the span of distances of start / end point / foot point to the point site indicate an intersection,
                        // we should find one.
                        assert(intersections.count > 0);
                        if (intersections.count == 2) {
                            // Now decide which points fall on this Voronoi edge.
                            // Tangential points (single intersection) are ignored.
                            if (possibly_two_points) {
                                Vec2d  v  = p1 - p0;
                                double l2 = v.squaredNorm();
                                double t0 = v.dot(intersections.pts[0] - p0);
                                double t1 = v.dot(intersections.pts[1] - p0);
                                if (t0 > t1) {
                                    std::swap(t0, t1);
                                    std::swap(intersections.pts[0], intersections.pts[1]);
                                }
                                // Remove points outside of the line range.
                                if (t0 < 0. || t0 > l2) {
                                    if (t1 < 0. || t1 > l2)
                                        intersections.count = 0;
                                    else {
                                        -- intersections.count;
                                        t0 = t1;
                                        intersections.pts[0] = intersections.pts[1];
                                    }
                                } else if (t1 < 0. || t1 > l2)
                                    -- intersections.count;
                            } else {
                                // Take the point furthest from the end points of the Voronoi edge or a Voronoi parabolic arc.
                                double d0 = std::max((intersections.pts[0] - p0).squaredNorm(), (intersections.pts[0] - p1).squaredNorm());
                                double d1 = std::max((intersections.pts[1] - p0).squaredNorm(), (intersections.pts[1] - p1).squaredNorm());
                                if (d0 > d1)
                                    intersections.pts[0] = intersections.pts[1];
                                -- intersections.count;
                            }
                            assert(intersections.count > 0);
                            if (intersections.count == 2) {
                                set_edge_state_final(edge_idx, EdgeState::Active);
                                set_edge_state_final(edge_idx2, EdgeState::Active);
                                edge_offset_point[edge_idx]  = intersections.pts[1];
                                edge_offset_point[edge_idx2] = intersections.pts[0];
                                done = true;
                            } else if (intersections.count == 1) {
                                if (d1 < d0)
                                    std::swap(edge_idx, edge_idx2);
                                set_edge_state_final(edge_idx, EdgeState::Active);
                                set_edge_state_final(edge_idx2, EdgeState::Inactive);
                                edge_offset_point[edge_idx] = intersections.pts[0];
                                done = true;
                            }
                        }
                    }
                }
                if (! done) {
                    set_edge_state_final(edge_idx, EdgeState::Inactive);
                    set_edge_state_final(edge_idx2, EdgeState::Inactive);
                }
            }
        }
    }

#ifndef NDEBUG
    for (const VD::edge_type &edge : vd.edges()) {
        assert(edge_state[&edge - front_edge] == EdgeState::Inactive || edge_state[&edge - front_edge] == EdgeState::Active);
        // None of a new edge candidate may start with null vertex.
        assert(edge_state[&edge - front_edge] == EdgeState::Inactive || edge.vertex0() != nullptr);
        assert(edge_state[edge.twin() - front_edge] == EdgeState::Inactive || edge.twin()->vertex0() != nullptr);
    }
#endif // NDEBUG

#ifdef VORONOI_DEBUG_OUT
    {
        Lines helper_lines;
        for (const VD::edge_type &edge : vd.edges())
            if (edge_state[&edge - front_edge] == EdgeState::Active)
                helper_lines.emplace_back(Line(Point(edge.vertex0()->x(), edge.vertex0()->y()), Point(edge_offset_point[&edge - front_edge].cast<coord_t>())));
        dump_voronoi_to_svg(debug_out_path("voronoi-offset-candidates2-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), helper_lines);
    }
#endif // VORONOI_DEBUG_OUT

    auto next_offset_edge = [&edge_state, front_edge](const VD::edge_type *start_edge) -> const VD::edge_type* {
	    for (const VD::edge_type *edge = start_edge->next(); edge != start_edge; edge = edge->next())
            if (edge_state[edge->twin() - front_edge] == EdgeState::Active)
                return edge->twin();
        // assert(false);
        return nullptr;
	};

#ifndef NDEBUG
	auto dist_to_site = [&lines](const VD::cell_type &cell, const Vec2d &point) {
        const Line &line = lines[cell.source_index()];
        return cell.contains_point() ?
            (((cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line.a : line.b).cast<double>() - point).norm() :
            (Geometry::foot_pt<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), point) - point).norm();
	};
#endif /* NDEBUG */

	// Track the offset curves.
	Polygons out;
	double angle_step    = 2. * acos((offset_distance - discretization_error) / offset_distance);
    double cos_threshold = cos(angle_step);
    for (size_t seed_edge_idx = 0; seed_edge_idx < vd.num_edges(); ++ seed_edge_idx)
		if (edge_state[seed_edge_idx] == EdgeState::Active) {
            const VD::edge_type *start_edge = &vd.edges()[seed_edge_idx];
            const VD::edge_type *edge       = start_edge;
            Polygon  			 poly;
		    do {
		        // find the next edge
                const VD::edge_type *next_edge = next_offset_edge(edge);
#ifdef VORONOI_DEBUG_OUT
                if (next_edge == nullptr) {
                    Lines helper_lines;
                    dump_voronoi_to_svg(debug_out_path("voronoi-offset-open-loop-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), to_lines(poly));
                }
#endif // VORONOI_DEBUG_OUT
                assert(next_edge);
		        //std::cout << "offset-output: "; print_edge(edge); std::cout << " to "; print_edge(next_edge); std::cout << "\n";
		        // Interpolate a circular segment or insert a linear segment between edge and next_edge.
                const VD::cell_type  *cell      = edge->cell();
                edge_state[next_edge - front_edge] = EdgeState::Inactive;
                Vec2d p1 = edge_offset_point[edge - front_edge];
                Vec2d p2 = edge_offset_point[next_edge - front_edge];
#ifndef NDEBUG
                {
                    double err  = dist_to_site(*cell, p1) - offset_distance;
                    double err2 = dist_to_site(*cell, p2) - offset_distance;
#ifdef VORONOI_DEBUG_OUT
                    if (std::max(err, err2) >= SCALED_EPSILON) {
                        Lines helper_lines;
                        dump_voronoi_to_svg(debug_out_path("voronoi-offset-incorrect_pt-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), to_lines(poly));
                    }
#endif // VORONOI_DEBUG_OUT
                    assert(std::abs(err) < SCALED_EPSILON);
                    assert(std::abs(err2) < SCALED_EPSILON);
                }
#endif /* NDEBUG */
				if (cell->contains_point()) {
					// Discretize an arc from p1 to p2 with radius = offset_distance and discretization_error.
                    // The extracted contour is CCW oriented, extracted holes are CW oriented.
                    // The extracted arc will have the same orientation. As the Voronoi regions are convex, the angle covered by the arc will be convex as well.
                    const Line  &line0  = lines[cell->source_index()];
					const Vec2d &center = ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b).cast<double>();
					const Vec2d  v1 	= p1 - center;
					const Vec2d  v2 	= p2 - center;
                    bool 		 ccw    = cross2(v1, v2) > 0;
                    double       cos_a  = v1.dot(v2);
                    double       norm   = v1.norm() * v2.norm();
                    assert(norm > 0.);
                    if (cos_a < cos_threshold * norm) {
						// Angle is bigger than the threshold, therefore the arc will be discretized.
                        cos_a /= norm;
                        assert(cos_a > -1. - EPSILON && cos_a < 1. + EPSILON);
                        double angle = acos(std::max(-1., std::min(1., cos_a)));
						size_t n_steps = size_t(ceil(angle / angle_step));
						double astep = angle / n_steps;
						if (! ccw)
							astep *= -1.;
						double a = astep;
						for (size_t i = 1; i < n_steps; ++ i, a += astep) {
							double c = cos(a);
							double s = sin(a);
							Vec2d  p = center + Vec2d(c * v1.x() - s * v1.y(), s * v1.x() + c * v1.y());
                            poly.points.emplace_back(Point(coord_t(p.x()), coord_t(p.y())));
						}
                    }
				}
                {
                    Point pt_last(coord_t(p2.x()), coord_t(p2.y()));
                    if (poly.empty() || poly.points.back() != pt_last)
                        poly.points.emplace_back(pt_last);
                }
                edge = next_edge;
		    } while (edge != start_edge);
		    out.emplace_back(std::move(poly));
		}

	return out;
}
#else

Polygons offset(
	const VD 		&vd, 
	const Lines 	&lines, 
	double 			 offset_distance, 
	double 			 discretization_error)
{
    annotate_inside_outside(const_cast<VD&>(vd), lines);
    std::vector<double> dist = signed_vertex_distances(vd, lines);
    return offset(vd, lines, dist, offset_distance, discretization_error);
}

#endif

} // namespace Voronoi
} // namespace Slic3r