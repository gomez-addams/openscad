/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef _MSC_VER 
// Boost conflicts with MPFR under MSVC (google it)
#include <mpfr.h>
#endif

// dxfdata.h must come first for Eigen SIMD alignment issues
#include "dxfdata.h"
#include "polyset.h"
#include "polyset-utils.h"
#include "printutils.h"
#include "feature.h"

#include "CGALRenderer.h"

//#include "Preferences.h"

CGALRenderer::CGALRenderer(shared_ptr<const class Geometry> geom)
	: last_render_state(Feature::ExperimentalVxORenderers.is_enabled()), polyset_vbo(0) // FIXME: this is temporary to make switching between renderers seamless.
{
	this->addGeometry(geom);
}

void CGALRenderer::addGeometry(const shared_ptr<const Geometry> &geom)
{
	if (const auto geomlist = dynamic_pointer_cast<const GeometryList>(geom)) {
		for (const auto &item : geomlist->getChildren()) {
			this->addGeometry(item.second);
		}
	}
	else if (const auto ps = dynamic_pointer_cast<const PolySet>(geom)) {
		assert(ps->getDimension() == 3);
		// We need to tessellate here, in case the generated PolySet contains concave polygons
		// See testdata/scad/3D/features/polyhedron-concave-test.scad
		auto ps_tri = new PolySet(3, ps->convexValue());
		ps_tri->setConvexity(ps->getConvexity());
		PolysetUtils::tessellate_faces(*ps, *ps_tri);
		this->polysets.push_back(shared_ptr<const PolySet>(ps_tri));
	}
	else if (const auto poly = dynamic_pointer_cast<const Polygon2d>(geom)) {
		this->polysets.push_back(shared_ptr<const PolySet>(poly->tessellate()));
	}
	else if (const auto new_N = dynamic_pointer_cast<const CGAL_Nef_polyhedron>(geom)) {
		assert(new_N->getDimension() == 3);
		if (!new_N->isEmpty()) {
			this->nefPolyhedrons.push_back(new_N);
		}
	}
}

CGALRenderer::~CGALRenderer()
{
	if (polyset_vbo) glDeleteBuffers(1, &polyset_vbo);
}

const std::list<shared_ptr<class CGAL_OGL_Polyhedron> > &CGALRenderer::getPolyhedrons() const
{
	if (!this->nefPolyhedrons.empty() &&
	    (this->polyhedrons.empty() || Feature::ExperimentalVxORenderers.is_enabled() != last_render_state)) // FIXME: this is temporary to make switching between renderers seamless.
	    buildPolyhedrons();
	return this->polyhedrons;
}

void CGALRenderer::buildPolyhedrons() const
{
	PRINTD("buildPolyhedrons");
	if (!Feature::ExperimentalVxORenderers.is_enabled()) {
		this->polyhedrons.clear();

		for(const auto &N : this->nefPolyhedrons) {
			auto p = new CGAL_OGL_Polyhedron(*this->colorscheme);
			CGAL::OGL::Nef3_Converter<CGAL_Nef_polyhedron3>::convert_to_OGLPolyhedron(*N->p3, p);
			// CGAL_NEF3_MARKED_FACET_COLOR <- CGAL_FACE_BACK_COLOR
			// CGAL_NEF3_UNMARKED_FACET_COLOR <- CGAL_FACE_FRONT_COLOR
			p->init();
			this->polyhedrons.push_back(shared_ptr<CGAL_OGL_Polyhedron>(p));
		}
	} else {
		this->polyhedrons.clear();

		for(const auto &N : this->nefPolyhedrons) {
			auto p = new CGAL_OGL_VBOPolyhedron(*this->colorscheme);
			CGAL::OGL::Nef3_Converter<CGAL_Nef_polyhedron3>::convert_to_OGLPolyhedron(*N->p3, p);
			// CGAL_NEF3_MARKED_FACET_COLOR <- CGAL_FACE_BACK_COLOR
			// CGAL_NEF3_UNMARKED_FACET_COLOR <- CGAL_FACE_FRONT_COLOR
			p->init();
			this->polyhedrons.push_back(shared_ptr<CGAL_OGL_Polyhedron>(p));
		}
	}
	PRINTD("buildPolyhedrons() end");
}

// Overridden from Renderer
void CGALRenderer::setColorScheme(const ColorScheme &cs)
{
	PRINTD("setColorScheme");
	Renderer::setColorScheme(cs);
	colormap[ColorMode::CGAL_FACE_2D_COLOR] = ColorMap::getColor(cs, RenderColor::CGAL_FACE_2D_COLOR);
	colormap[ColorMode::CGAL_EDGE_2D_COLOR] = ColorMap::getColor(cs, RenderColor::CGAL_EDGE_2D_COLOR);
	this->polyhedrons.clear(); // Mark as dirty
	PRINTD("setColorScheme done");
}

void CGALRenderer::createPolysets() const
{
	PRINTD("createPolysets() polyset");
	
	polyset_states.clear();

	VertexArray vertex_array(std::make_unique<VertexStateFactory>(), polyset_states);
	
	// POLYSET_2D_DATA
	std::shared_ptr<VertexData> vertex_data = std::make_shared<VertexData>();
	vertex_data->addPositionData(std::make_shared<AttributeData<GLfloat,3,GL_FLOAT>>());
	vertex_data->addColorData(std::make_shared<AttributeData<GLfloat,4,GL_FLOAT>>());
	vertex_array.addVertexData(vertex_data);

	// POLYSET_3D_DATA
	vertex_data = std::make_shared<VertexData>();
	vertex_data->addPositionData(std::make_shared<AttributeData<GLfloat,3,GL_FLOAT>>());
	vertex_data->addNormalData(std::make_shared<AttributeData<GLfloat,3,GL_FLOAT>>());
	vertex_data->addColorData(std::make_shared<AttributeData<GLfloat,4,GL_FLOAT>>());
	vertex_array.addVertexData(vertex_data);
	
	for (const auto &polyset : this->polysets) {
		Color4f color;

		PRINTD("polysets");
		if (polyset->getDimension() == 2) {
			PRINTD("2d polysets");
			vertex_array.writeIndex(POLYSET_2D_DATA);

			std::shared_ptr<VertexState> init_state = std::make_shared<VertexState>();
			init_state->glEnd().emplace_back([]() {
				if (OpenSCAD::debug != "") PRINTD("glDisable(GL_LIGHTING)");
				glDisable(GL_LIGHTING);
			});
			polyset_states.emplace_back(std::move(init_state));

			// Create 2D polygons
			getColor(ColorMode::CGAL_FACE_2D_COLOR, color);
			this->create_polygons(polyset.get(), vertex_array, CSGMODE_NONE, Transform3d::Identity(), color);

			std::shared_ptr<VertexState> edge_state = std::make_shared<VertexState>();
			edge_state->glBegin().emplace_back([]() {
				if (OpenSCAD::debug != "") PRINTD("glDisable(GL_DEPTH_TEST)");
				glDisable(GL_DEPTH_TEST);
			});
			edge_state->glBegin().emplace_back([]() {
				if (OpenSCAD::debug != "") PRINTD("glLineWidth(2)");
				glLineWidth(2);
			});
			polyset_states.emplace_back(std::move(edge_state));
			
			// Create 2D edges
			getColor(ColorMode::CGAL_EDGE_2D_COLOR, color);
			this->create_edges(polyset.get(), vertex_array, CSGMODE_NONE, Transform3d::Identity(), color);
			
			std::shared_ptr<VertexState> end_state = std::make_shared<VertexState>();
			end_state->glBegin().emplace_back([]() {
				if (OpenSCAD::debug != "") PRINTD("glEnable(GL_DEPTH_TEST)");
				glEnable(GL_DEPTH_TEST);
			});
			polyset_states.emplace_back(std::move(end_state));
		} else {
			PRINTD("3d polysets");
			vertex_array.writeIndex(POLYSET_3D_DATA);

			// Create 3D polygons
			getColor(ColorMode::MATERIAL, color);
			this->create_surface(polyset.get(), vertex_array, CSGMODE_NORMAL, Transform3d::Identity(), color);
		}
	}
	
	if (this->polysets.size()) {
		glGenBuffers(1, &polyset_vbo);
		vertex_array.createInterleavedVBO(polyset_vbo);
	}
}

void CGALRenderer::draw(bool showfaces, bool showedges, const shaderinfo_t * /*shaderinfo*/) const
{
	PRINTD("draw()");
	if (!Feature::ExperimentalVxORenderers.is_enabled()) {
		for (const auto &polyset : this->polysets) {
			PRINTD("draw() polyset");
			if (polyset->getDimension() == 2) {
				// Draw 2D polygons
				glDisable(GL_LIGHTING);
				setColor(ColorMode::CGAL_FACE_2D_COLOR);
				
				for (const auto &polygon : polyset->polygons) {
					glBegin(GL_POLYGON);
					for (const auto &p : polygon) {
						glVertex3d(p[0], p[1], 0);
					}
					glEnd();
				}

				// Draw 2D edges
				glDisable(GL_DEPTH_TEST);

				glLineWidth(2);
				setColor(ColorMode::CGAL_EDGE_2D_COLOR);
				this->render_edges(polyset, CSGMODE_NONE);
				glEnable(GL_DEPTH_TEST);
			}
			else {
				// Draw 3D polygons
				setColor(ColorMode::MATERIAL);
				this->render_surface(polyset, CSGMODE_NORMAL, Transform3d::Identity(), nullptr);
			}
		}
	} else {
		PRINTDB("product_vertex_sets.size = %d", polyset_states.size());
		if (!polyset_states.size()) createPolysets();
		
		// grab current state to restore after
		GLfloat current_point_size, current_line_width;
		GLboolean origVertexArrayState = glIsEnabled(GL_VERTEX_ARRAY);
		GLboolean origNormalArrayState = glIsEnabled(GL_NORMAL_ARRAY);
		GLboolean origColorArrayState = glIsEnabled(GL_COLOR_ARRAY);

		glGetFloatv(GL_POINT_SIZE, &current_point_size);
		glGetFloatv(GL_LINE_WIDTH, &current_line_width);

		glBindBuffer(GL_ARRAY_BUFFER, polyset_vbo);

		for (const auto &polyset : polyset_states) {
			if (polyset) polyset->drawArrays();
		}
		
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		// restore states
		glPointSize(current_point_size);
		glLineWidth(current_line_width);

		if (!origVertexArrayState) glDisableClientState(GL_VERTEX_ARRAY);
		if (!origNormalArrayState) glDisableClientState(GL_NORMAL_ARRAY);
		if (!origColorArrayState) glDisableClientState(GL_COLOR_ARRAY);
	}

	for (const auto &p : this->getPolyhedrons()) {
		*const_cast<bool *>(&last_render_state) = Feature::ExperimentalVxORenderers.is_enabled(); // FIXME: this is temporary to make switching between renderers seamless.
		if (showfaces) p->set_style(SNC_BOUNDARY);
		else p->set_style(SNC_SKELETON);
		p->draw(showfaces && showedges);
	}

	PRINTD("draw() end");
}

BoundingBox CGALRenderer::getBoundingBox() const
{
	BoundingBox bbox;

  	for (const auto &p : this->getPolyhedrons()) {
		CGAL::Bbox_3 cgalbbox = p->bbox();
		bbox.extend(BoundingBox(
			Vector3d(cgalbbox.xmin(), cgalbbox.ymin(), cgalbbox.zmin()),
			Vector3d(cgalbbox.xmax(), cgalbbox.ymax(), cgalbbox.zmax())));
	}
	for (const auto &ps : this->polysets) {
		bbox.extend(ps->getBoundingBox());
	}
	return bbox;
}
