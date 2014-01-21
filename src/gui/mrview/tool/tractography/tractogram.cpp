/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier & David Raffelt 17/12/12

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <QMenu>
#include <stdio.h>

#include "progressbar.h"
#include "image/stride.h"
#include "gui/mrview/tool/tractography/tractogram.h"
#include "gui/mrview/window.h"
#include "gui/projection.h"
#include "dwi/tractography/file.h"
#include "gui/opengl/lighting.h"



const size_t MAX_BUFFER_SIZE = 2796200;  // number of points to fill 32MB

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        std::string Tractogram::Shader::vertex_shader_source (const Displayable& tractogram)
        {
          bool colour_by_direction = (color_type == Direction);

          std::string source =
              "layout (location = 0) in vec3 vertexPosition_modelspace;\n"
              "layout (location = 1) in vec3 previousVertex;\n"
              "layout (location = 2) in vec3 nextVertex;\n"
              "uniform mat4 MVP;\n"
              "flat out float amp_out;\n"
              "out vec3 fragmentColour;\n";
          if (use_lighting) 
            source += 
              "out vec3 tangent;\n"
              "uniform mat4 MV;\n";
              
          switch (color_type) {
            case Direction: break;
            case Ends:
              source += "layout (location = 3) in vec3 color;\n";
              break;
            case Colour:
              source += "uniform vec3 const_colour;\n";
              break;
          }

          if (do_crop_to_slab) {
            source +=
                "out float include;\n"
                "uniform vec3 screen_normal;\n"
                "uniform float crop_var;\n"
                "uniform float slab_width;\n";
          }

          source +=
              "void main() {\n"
              "  gl_Position =  MVP * vec4(vertexPosition_modelspace,1);\n";

          if (use_lighting || colour_by_direction) 
            source += 
              "  vec3 dir;\n"
              "  if (isnan (previousVertex.x))\n"
              "    dir = nextVertex - vertexPosition_modelspace;\n"
              "  else if (isnan (nextVertex.x))\n"
              "    dir = vertexPosition_modelspace - previousVertex;\n"
              "  else\n"
              "    dir = nextVertex - previousVertex;\n";
          if (colour_by_direction)
              source += "  fragmentColour = dir;\n";
          if (use_lighting)
              source += "  tangent = normalize (mat3(MV) * dir);\n";

          switch (color_type) {
            case Ends:
              source += std::string (" fragmentColour = color;\n");
              break;
            case Colour:
              source +=
                  "  fragmentColour = const_colour;\n";
              break;
            default:
              break;
          }

          if (do_crop_to_slab)
            source +=
                "  include = (dot (vertexPosition_modelspace, screen_normal) - crop_var) / slab_width;\n";

          source += "}\n";

          return source;
        }



        std::string Tractogram::Shader::fragment_shader_source (const Displayable& tractogram) 
        {
          bool colour_by_direction = (color_type == Direction);

          std::string source =
              "in float include; \n"
              "out vec3 color;\n"
              "flat in float amp_out;\n"
              "in vec3 fragmentColour;\n";
          if (use_lighting)
            source += 
              "in vec3 tangent;\n"
              "uniform float ambient, diffuse, specular, shine;\n"
              "uniform vec3 light_pos;\n";

          source +=
              "void main(){\n";

          if (do_crop_to_slab)
            source += "  if (include < 0 || include > 1) discard;\n";

          if (use_lighting)
            source += "  vec3 t = normalize (tangent);\n";

          source +=
            std::string("  color = ") + (colour_by_direction ? "normalize (abs (fragmentColour))" : "fragmentColour" ) + ";\n";

          if (use_lighting) 
            source += 
             "  float l_dot_t = dot(light_pos, t);\n"
             "  vec3 l_perp = light_pos - l_dot_t * t;\n"
             "  vec3 l_perp_norm = normalize (l_perp);\n"
             "  float n_dot_t = t.z;\n"
             "  vec3 n_perp = vec3(0.0, 0.0, 1.0) - n_dot_t * t;\n"
             "  vec3 n_perp_norm = normalize (n_perp);\n"
             "  float cos2_theta = 0.5+0.5*dot(l_perp_norm,n_perp_norm);\n"
             "  color *= ambient + diffuse * length(l_perp) * cos2_theta;\n"
             "  color += specular * sqrt(cos2_theta) * pow (clamp (-l_dot_t*n_dot_t + length(l_perp)*length(n_perp), 0, 1), shine);\n";

          source += "}\n";

          return source;
        }


        bool Tractogram::Shader::need_update (const Displayable& object) const
        {
          const Tractogram& tractogram (dynamic_cast<const Tractogram&> (object));
          if (do_crop_to_slab != tractogram.tractography_tool.crop_to_slab() ||
              color_type != tractogram.color_type) 
            return true;
          if (use_lighting != tractogram.tractography_tool.use_lighting)
            return true;
          return Displayable::Shader::need_update (object);
        }




        void Tractogram::Shader::update (const Displayable& object) 
        {
          const Tractogram& tractogram (dynamic_cast<const Tractogram&> (object));
          do_crop_to_slab = tractogram.tractography_tool.crop_to_slab();
          use_lighting = tractogram.tractography_tool.use_lighting;
          color_type = tractogram.color_type;
          Displayable::Shader::update (object);
        }







        Tractogram::Tractogram (Window& window, Tractography& tool, const std::string& filename) :
            Displayable (filename),
            color_type (Direction),
            window (window),
            tractography_tool (tool),
            filename (filename)
        {
          set_allowed_features (true, true, true);
          colourmap = 1;
        }





        Tractogram::~Tractogram ()
        {
          if (vertex_buffers.size())
            glDeleteBuffers (vertex_buffers.size(), &vertex_buffers[0]);
          if (vertex_array_objects.size())
            glDeleteVertexArrays (vertex_array_objects.size(), &vertex_array_objects[0]);
          if (colour_buffers.size())
            glDeleteBuffers (colour_buffers.size(), &colour_buffers[0]);
        }





        void Tractogram::render (const Projection& transform)
        {
          if (tractography_tool.do_crop_to_slab && tractography_tool.slab_thickness <= 0.0)
            return;

          start (track_shader);
          transform.set (track_shader);

          if (tractography_tool.do_crop_to_slab) {
            glUniform3f (glGetUniformLocation (track_shader, "screen_normal"),
                transform.screen_normal()[0], transform.screen_normal()[1], transform.screen_normal()[2]);
            glUniform1f (glGetUniformLocation (track_shader, "crop_var"),
                window.focus().dot(transform.screen_normal()) - tractography_tool.slab_thickness / 2);
            glUniform1f (glGetUniformLocation (track_shader, "slab_width"),
                tractography_tool.slab_thickness);
          }

          if (color_type == Colour) 
            glUniform3fv (glGetUniformLocation (track_shader, "const_colour"), 1, colour);

          if (tractography_tool.use_lighting) {
            glUniformMatrix4fv (glGetUniformLocation (track_shader, "MV"), 1, GL_FALSE, transform.modelview());
            glUniform3fv (glGetUniformLocation (track_shader, "light_pos"), 1, tractography_tool.lighting->lightpos);
            glUniform1f (glGetUniformLocation (track_shader, "ambient"), tractography_tool.lighting->ambient);
            glUniform1f (glGetUniformLocation (track_shader, "diffuse"), tractography_tool.lighting->diffuse);
            glUniform1f (glGetUniformLocation (track_shader, "specular"), tractography_tool.lighting->specular);
            glUniform1f (glGetUniformLocation (track_shader, "shine"), tractography_tool.lighting->shine);
          }

          if (tractography_tool.line_opacity < 1.0) {
            glEnable (GL_BLEND);
            glDisable (GL_DEPTH_TEST);
            glDepthMask (GL_FALSE);
            glBlendEquation (GL_FUNC_ADD);
            glBlendFunc (GL_CONSTANT_ALPHA, GL_ONE);
            glBlendColor (1.0, 1.0, 1.0, tractography_tool.line_opacity);
          } else {
            glDisable (GL_BLEND);
            glEnable (GL_DEPTH_TEST);
            glDepthMask (GL_TRUE);
          }

          glLineWidth (tractography_tool.line_thickness);

          for (size_t buf = 0; buf < vertex_buffers.size(); ++buf) {
            glBindVertexArray (vertex_array_objects[buf]);
            glMultiDrawArrays (GL_LINE_STRIP, &track_starts[buf][0], &track_sizes[buf][0], num_tracks_per_buffer[buf]);
          }

          if (tractography_tool.line_opacity < 1.0) {
            glDisable (GL_BLEND);
            glEnable (GL_DEPTH_TEST);
            glDepthMask (GL_TRUE);
          }

          stop (track_shader);
        }








        void Tractogram::load_tracks()
        {
          DWI::Tractography::Reader<float> file (filename, properties);
          std::vector<Point<float> > tck;
          std::vector<Point<float> > buffer;
          std::vector<GLint> starts;
          std::vector<GLint> sizes;
          size_t tck_count = 0;

          while (file.next (tck)) {
            starts.push_back (buffer.size());
            buffer.push_back (Point<float>());
            buffer.insert (buffer.end(), tck.begin(), tck.end());
            sizes.push_back(tck.size());
            tck_count++;
            if (buffer.size() >= MAX_BUFFER_SIZE)
              load_tracks_onto_GPU (buffer, starts, sizes, tck_count);
          }
          if (buffer.size())
            load_tracks_onto_GPU (buffer, starts, sizes, tck_count);
          file.close();
        }
        
        
        
        
        void Tractogram::load_end_colours()
        {
          erase_nontrack_data();
          // TODO Is it possible to read the track endpoints from the GPU buffer rather than re-reading the .tck file?
          DWI::Tractography::Reader<float> file (filename, properties);
          for (size_t buffer_index = 0; buffer_index != vertex_buffers.size(); ++buffer_index) {
            size_t num_tracks = num_tracks_per_buffer[buffer_index];
            std::vector< Point<float> > buffer;
            std::vector< Point<float> > tck;
            while (num_tracks--) {
              file.next (tck);
              const Point<float> tangent ((tck.back() - tck.front()).normalise());
              const Point<float> colour (Math::abs (tangent[0]), Math::abs (tangent[1]), Math::abs (tangent[2]));
              buffer.push_back (Point<float>());
              for (std::vector< Point<float> >::iterator i = tck.begin(); i != tck.end(); ++i)
                *i = colour;
              buffer.insert (buffer.end(), tck.begin(), tck.end());
            }
            load_end_colours_onto_GPU (buffer);
          }
          file.close();
        }




        void Tractogram::erase_nontrack_data()
        {
          if (colour_buffers.size()) {
            glDeleteBuffers (colour_buffers.size(), &colour_buffers[0]);
            colour_buffers.clear();
          }
        }




        void Tractogram::load_tracks_onto_GPU (std::vector<Point<float> >& buffer,
            std::vector<GLint>& starts,
            std::vector<GLint>& sizes,
            size_t& tck_count)
        {
          buffer.push_back (Point<float>());
          GLuint vertexbuffer;
          glGenBuffers (1, &vertexbuffer);
          glBindBuffer (GL_ARRAY_BUFFER, vertexbuffer);
          glBufferData (GL_ARRAY_BUFFER, buffer.size() * sizeof(Point<float>), &buffer[0][0], GL_STATIC_DRAW);

          GLuint vertex_array_object;
          glGenVertexArrays (1, &vertex_array_object);
          glBindVertexArray (vertex_array_object);
          glEnableVertexAttribArray (0);
          glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 0, (void*)(3*sizeof(float)));
          glEnableVertexAttribArray (1);
          glVertexAttribPointer (1, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
          glEnableVertexAttribArray (2);
          glVertexAttribPointer (2, 3, GL_FLOAT, GL_FALSE, 0, (void*)(6*sizeof(float)));

          vertex_array_objects.push_back (vertex_array_object);
          vertex_buffers.push_back (vertexbuffer);
          track_starts.push_back (starts);
          track_sizes.push_back (sizes);
          num_tracks_per_buffer.push_back (tck_count);
          buffer.clear();
          starts.clear();
          sizes.clear();
          tck_count = 0;
        }
        
        
        
        
        
        void Tractogram::load_end_colours_onto_GPU (std::vector< Point<float> >& buffer) {
          buffer.push_back (Point<float>());
          GLuint vertexbuffer;
          glGenBuffers (1, &vertexbuffer);
          glBindBuffer (GL_ARRAY_BUFFER, vertexbuffer);
          glBufferData (GL_ARRAY_BUFFER, buffer.size() * sizeof(Point<float>), &buffer[0][0], GL_STATIC_DRAW);

          glBindVertexArray (vertex_array_objects[colour_buffers.size()]);
          glEnableVertexAttribArray (3);
          glVertexAttribPointer (3, 3, GL_FLOAT, GL_FALSE, 0, (void*)(3*sizeof(float)));

          colour_buffers.push_back (vertexbuffer);
          buffer.clear();
        }




      }
    }
  }
}


