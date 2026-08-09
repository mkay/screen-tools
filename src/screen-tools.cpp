/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Scott Moreau (original crosshair plugin)
 * Copyright (c) 2026 Modified for screen guides
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>

enum measure_state_t
{
    MEASURE_IDLE,
    MEASURE_ACTIVE,
    MEASURE_FROZEN,
};

class wayfire_screen_tools : public wf::per_output_plugin_instance_t
{
    wf::option_wrapper_t<int> line_width{"screen-tools/line_width"};
    wf::option_wrapper_t<wf::color_t> line_color{"screen-tools/line_color"};
    wf::option_wrapper_t<wf::activatorbinding_t> toggle_binding{"screen-tools/toggle"};
    wf::option_wrapper_t<wf::activatorbinding_t> cancel_binding{"screen-tools/cancel"};
    wf::option_wrapper_t<wf::buttonbinding_t> freeze_binding{"screen-tools/freeze"};
    wf::option_wrapper_t<wf::buttonbinding_t> measure_binding{"screen-tools/measure"};
    wf::option_wrapper_t<wf::color_t> measure_color_opt{"screen-tools/measure_color"};
    wf::option_wrapper_t<int> measure_font_size{"screen-tools/measure_font_size"};
    wf::option_wrapper_t<int> crosshair_measure_opacity{"screen-tools/crosshair_measure_opacity"};

    // Picker options
    wf::option_wrapper_t<wf::buttonbinding_t> picker_binding{"screen-tools/picker"};
    wf::option_wrapper_t<std::string> picker_format{"screen-tools/picker_format"};
    wf::option_wrapper_t<int> picker_loupe_radius{"screen-tools/picker_loupe_radius"};
    wf::option_wrapper_t<int> picker_cell_size{"screen-tools/picker_cell_size"};

    wf::geometry_t geometry[2];
    bool is_active = false;
    bool is_frozen = false;
    wf::pointf_t frozen_position{0, 0};

    OpenGL::program_t program;

    // Measurement state
    measure_state_t measure_state = MEASURE_IDLE;
    wf::pointf_t measure_point_a{0, 0};
    wf::pointf_t measure_point_b{0, 0};
    wf::pointf_t last_measure_b{-1, -1};
    wf::geometry_t measure_rect_geom[4]; // top, bottom, left, right outline rects
    wf::geometry_t measure_label_geom{0, 0, 0, 0};

    cairo_t *measure_cr = nullptr;
    cairo_surface_t *measure_surface = nullptr;
    wf::owned_texture_t measure_tex;

    // Picker state
    bool picker_active = false;
    std::vector<uint8_t> picker_pixel_data;
    uint8_t picked_color[4] = {0, 0, 0, 255}; // RGBA
    cairo_t *picker_cr = nullptr;
    cairo_surface_t *picker_surface = nullptr;
    wf::owned_texture_t picker_tex;
    wf::geometry_t loupe_geom{0, 0, 0, 0};
    wf::geometry_t prev_loupe_geom{0, 0, 0, 0};
    bool picker_needs_read = false;

  public:
    void init() override
    {
        output->add_activator(toggle_binding, &toggle_cb);
        output->add_activator(cancel_binding, &cancel_cb);
        output->add_button(freeze_binding, &freeze_cb);
        output->add_button(measure_binding, &measure_cb);
        output->add_button(picker_binding, &picker_cb);
    }

    wf::activator_callback toggle_cb = [=] (auto)
    {
        is_active = !is_active;

        if (is_active)
        {
            output->render->add_effect(&post_hook, wf::OUTPUT_EFFECT_OVERLAY);
            output->render->add_effect(&frame_pre_paint, wf::OUTPUT_EFFECT_DAMAGE);
            output->render->damage_whole();
        }
        else
        {
            is_frozen = false;
            cleanup_measure();
            cleanup_picker();
            output->render->rem_effect(&post_hook);
            output->render->rem_effect(&frame_pre_paint);
            output->render->damage_whole();
        }

        return true;
    };

    wf::activator_callback cancel_cb = [=] (auto)
    {
        if (!is_active)
        {
            return false;
        }

        bool handled = false;

        if (picker_active)
        {
            cleanup_picker();
            handled = true;
        }

        if (measure_state != MEASURE_IDLE)
        {
            damage_measure();
            cleanup_measure();
            handled = true;
        }

        return handled;
    };

    wf::button_callback freeze_cb = [=] (auto)
    {
        if (!is_active)
        {
            return false;
        }

        is_frozen = !is_frozen;

        if (is_frozen)
        {
            frozen_position = output->get_cursor_position();
        }

        output->render->damage(geometry[0]);
        output->render->damage(geometry[1]);

        return true;
    };

    wf::button_callback measure_cb = [=] (auto)
    {
        if (!is_active)
        {
            return false;
        }

        // Exit freeze so measurement uses real cursor position
        if (is_frozen)
        {
            is_frozen = false;
            output->render->damage(geometry[0]);
            output->render->damage(geometry[1]);
        }

        auto click = output->get_cursor_position();

        switch (measure_state)
        {
          case MEASURE_IDLE:
            measure_point_a = click;
            measure_point_b = click;
            last_measure_b  = {-1, -1};
            measure_state   = MEASURE_ACTIVE;
            compute_measure_rect();
            render_measure_label();
            damage_measure();
            break;

          case MEASURE_ACTIVE:
            measure_point_b = click;
            measure_state   = MEASURE_FROZEN;
            compute_measure_rect();
            render_measure_label();
            damage_measure();
            break;

          case MEASURE_FROZEN:
            damage_measure();
            cleanup_measure();
            break;
        }

        return true;
    };

    wf::button_callback picker_cb = [=] (auto)
    {
        if (!is_active)
        {
            return false;
        }

        if (!picker_active)
        {
            // Enter picker mode
            picker_active = true;
            picker_needs_read = true;
            output->render->set_redraw_always(true);
            output->render->damage_whole();
        }
        else
        {
            // Second press: pick color and copy to clipboard
            std::string color_str = format_color();
            copy_to_clipboard(color_str.c_str());
            cleanup_picker();
        }

        return true;
    };

    void copy_to_clipboard(const char *text)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            // Child process: exec wl-copy
            execlp("wl-copy", "wl-copy", text, (char *)nullptr);
            _exit(127); // exec failed
        }
        // Parent: fire-and-forget (child will be reaped by init)
    }

    std::string format_color()
    {
        uint8_t R = picked_color[0];
        uint8_t G = picked_color[1];
        uint8_t B = picked_color[2];

        std::string fmt = picker_format;

        if (fmt == "rgb")
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)", R, G, B);
            return buf;
        }

        if (fmt == "hsl")
        {
            double r = R / 255.0, g = G / 255.0, b = B / 255.0;
            double cmax = std::max({r, g, b});
            double cmin = std::min({r, g, b});
            double delta = cmax - cmin;
            double h = 0, s = 0, l = (cmax + cmin) / 2.0;

            if (delta > 0)
            {
                s = (l > 0.5) ? delta / (2.0 - cmax - cmin)
                              : delta / (cmax + cmin);
                if (cmax == r)
                {
                    h = std::fmod((g - b) / delta + 6.0, 6.0) * 60.0;
                }
                else if (cmax == g)
                {
                    h = ((b - r) / delta + 2.0) * 60.0;
                }
                else
                {
                    h = ((r - g) / delta + 4.0) * 60.0;
                }
            }

            char buf[40];
            std::snprintf(buf, sizeof(buf), "hsl(%.0f, %.0f%%, %.0f%%)",
                h, s * 100.0, l * 100.0);
            return buf;
        }

        if (fmt == "oklch")
        {
            // sRGB → linear sRGB
            auto to_linear = [](double v) -> double {
                return (v <= 0.04045) ? v / 12.92
                                      : std::pow((v + 0.055) / 1.055, 2.4);
            };

            double lr = to_linear(R / 255.0);
            double lg = to_linear(G / 255.0);
            double lb = to_linear(B / 255.0);

            // Linear sRGB → OKLab (Björn Ottosson's matrices)
            double l_ = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb;
            double m_ = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb;
            double s_ = 0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb;

            double l_c = std::cbrt(l_);
            double m_c = std::cbrt(m_);
            double s_c = std::cbrt(s_);

            double L = 0.2104542553 * l_c + 0.7936177850 * m_c - 0.0040720468 * s_c;
            double a = 1.9779984951 * l_c - 2.4285922050 * m_c + 0.4505937099 * s_c;
            double bk = 0.0259040371 * l_c + 0.7827717662 * m_c - 0.8086757660 * s_c;

            double C = std::sqrt(a * a + bk * bk);
            double H = std::atan2(bk, a) * 180.0 / M_PI;
            if (H < 0)
            {
                H += 360.0;
            }

            char buf[48];
            std::snprintf(buf, sizeof(buf), "oklch(%.3f %.3f %.1f)",
                L, C, H);
            return buf;
        }

        // Default: hex
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", R, G, B);
        return buf;
    }

    void compute_measure_rect()
    {
        double x1 = std::min(measure_point_a.x, measure_point_b.x);
        double y1 = std::min(measure_point_a.y, measure_point_b.y);
        double x2 = std::max(measure_point_a.x, measure_point_b.x);
        double y2 = std::max(measure_point_a.y, measure_point_b.y);

        double lw = std::max(1.0, (double)line_width);

        // top edge
        measure_rect_geom[0] = {x1, y1, x2 - x1 + lw, lw};
        // bottom edge
        measure_rect_geom[1] = {x1, y2, x2 - x1 + lw, lw};
        // left edge
        measure_rect_geom[2] = {x1, y1, lw, y2 - y1 + lw};
        // right edge
        measure_rect_geom[3] = {x2, y1, lw, y2 - y1 + lw};
    }

    void render_measure_label()
    {
        long w = std::lround(std::abs(measure_point_b.x - measure_point_a.x));
        long h = std::lround(std::abs(measure_point_b.y - measure_point_a.y));

        char text[64];
        std::snprintf(text, sizeof(text), "%ld x %ld", w, h);

        int font_size = measure_font_size;
        int padding   = 6;

        // Create a temporary surface to measure text
        if (!measure_cr)
        {
            measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            measure_cr = cairo_create(measure_surface);
        }

        cairo_select_font_face(measure_cr, "sans-serif",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(measure_cr, font_size);

        cairo_text_extents_t extents;
        cairo_text_extents(measure_cr, text, &extents);

        int surf_w = (int)(extents.width + padding * 2 + 2);
        int surf_h = (int)(extents.height + padding * 2 + 2);

        // Recreate surface at the correct size
        cairo_destroy(measure_cr);
        cairo_surface_destroy(measure_surface);

        measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
        measure_cr = cairo_create(measure_surface);

        // Clear
        cairo_set_source_rgba(measure_cr, 0, 0, 0, 0);
        cairo_set_operator(measure_cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(measure_cr);
        cairo_set_operator(measure_cr, CAIRO_OPERATOR_OVER);

        // Draw dark rounded-rect background
        double radius = 4.0;
        double x = 0.5, y = 0.5;
        double bw = surf_w - 1.0, bh = surf_h - 1.0;

        cairo_new_sub_path(measure_cr);
        cairo_arc(measure_cr, x + bw - radius, y + radius, radius, -M_PI / 2, 0);
        cairo_arc(measure_cr, x + bw - radius, y + bh - radius, radius, 0, M_PI / 2);
        cairo_arc(measure_cr, x + radius, y + bh - radius, radius, M_PI / 2, M_PI);
        cairo_arc(measure_cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
        cairo_close_path(measure_cr);

        cairo_set_source_rgba(measure_cr, 0, 0, 0, 0.8);
        cairo_fill(measure_cr);

        // Draw text
        cairo_select_font_face(measure_cr, "sans-serif",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(measure_cr, font_size);
        cairo_set_source_rgba(measure_cr, 1, 1, 1, 1);
        cairo_move_to(measure_cr,
            padding - extents.x_bearing,
            padding - extents.y_bearing);
        cairo_show_text(measure_cr, text);

        // Upload to GPU
        measure_tex = wf::owned_texture_t{measure_surface};

        // Position label centered below the rectangle, 8px gap
        double rx1 = std::min(measure_point_a.x, measure_point_b.x);
        double ry2 = std::max(measure_point_a.y, measure_point_b.y);
        double rx2 = std::max(measure_point_a.x, measure_point_b.x);

        double label_x = rx1 + (rx2 - rx1) / 2.0 - surf_w / 2.0;
        double label_y = ry2 + 8;

        // Clamp to output bounds
        auto og = output->get_relative_geometry();
        if (label_x < og.x)
        {
            label_x = og.x;
        }

        if (label_x + surf_w > og.x + og.width)
        {
            label_x = og.x + og.width - surf_w;
        }

        if (label_y + surf_h > og.y + og.height)
        {
            label_y = ry2 - surf_h - 8; // flip above
        }

        measure_label_geom = {label_x, label_y, (double)surf_w, (double)surf_h};
    }

    void damage_measure()
    {
        for (int i = 0; i < 4; i++)
        {
            output->render->damage(measure_rect_geom[i]);
        }

        output->render->damage(measure_label_geom);
    }

    void cleanup_measure()
    {
        measure_state  = MEASURE_IDLE;
        last_measure_b = {-1, -1};

        if (measure_cr)
        {
            cairo_destroy(measure_cr);
            measure_cr = nullptr;
        }

        if (measure_surface)
        {
            cairo_surface_destroy(measure_surface);
            measure_surface = nullptr;
        }

        measure_tex = wf::owned_texture_t{};
        measure_label_geom = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++)
        {
            measure_rect_geom[i] = {0, 0, 0, 0};
        }
    }

    void render_loupe()
    {
        int radius = picker_loupe_radius;
        int cell   = picker_cell_size;
        int grid   = 2 * radius + 1;
        int grid_px = grid * cell;

        // Format color label
        std::string color_label = format_color();

        int font_size = 12;
        int label_pad = 6;

        // Use a scratch context to measure text
        cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *tmp_cr = cairo_create(tmp);
        cairo_select_font_face(tmp_cr, "sans-serif",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(tmp_cr, font_size);
        cairo_text_extents_t text_ext;
        cairo_text_extents(tmp_cr, color_label.c_str(), &text_ext);
        cairo_destroy(tmp_cr);
        cairo_surface_destroy(tmp);

        int label_w = (int)(text_ext.width + label_pad * 2 + 2);
        int label_h = (int)(text_ext.height + label_pad * 2 + 2);
        int surf_w  = std::max(grid_px, label_w);
        int surf_h  = grid_px + 4 + label_h; // 4px gap between grid and label

        // Recreate picker surface
        if (picker_cr)
        {
            cairo_destroy(picker_cr);
        }

        if (picker_surface)
        {
            cairo_surface_destroy(picker_surface);
        }

        picker_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
        picker_cr = cairo_create(picker_surface);

        // Clear
        cairo_set_source_rgba(picker_cr, 0, 0, 0, 0);
        cairo_set_operator(picker_cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(picker_cr);
        cairo_set_operator(picker_cr, CAIRO_OPERATOR_OVER);

        // Draw outer background/border for the grid area
        double grid_x0 = (surf_w - grid_px) / 2.0;
        cairo_set_source_rgba(picker_cr, 0, 0, 0, 0.85);
        cairo_rectangle(picker_cr, grid_x0 - 1, 0 - 1, grid_px + 2, grid_px + 2);
        cairo_fill(picker_cr);

        // Draw each pixel cell
        for (int gy = 0; gy < grid; gy++)
        {
            for (int gx = 0; gx < grid; gx++)
            {
                int idx = (gy * grid + gx) * 4;
                double r = 0, g = 0, b = 0;
                if (idx + 2 < (int)picker_pixel_data.size())
                {
                    r = picker_pixel_data[idx + 0] / 255.0;
                    g = picker_pixel_data[idx + 1] / 255.0;
                    b = picker_pixel_data[idx + 2] / 255.0;
                }

                double cx = grid_x0 + gx * cell;
                double cy = gy * cell;

                cairo_set_source_rgb(picker_cr, r, g, b);
                cairo_rectangle(picker_cr, cx, cy, cell, cell);
                cairo_fill(picker_cr);
            }
        }

        // Draw grid lines
        cairo_set_source_rgba(picker_cr, 0, 0, 0, 0.3);
        cairo_set_line_width(picker_cr, 1.0);
        for (int i = 0; i <= grid; i++)
        {
            // Vertical
            double vx = grid_x0 + i * cell;
            cairo_move_to(picker_cr, vx, 0);
            cairo_line_to(picker_cr, vx, grid_px);
            // Horizontal
            double hy = i * cell;
            cairo_move_to(picker_cr, grid_x0, hy);
            cairo_line_to(picker_cr, grid_x0 + grid_px, hy);
        }

        cairo_stroke(picker_cr);

        // Highlight center cell
        double center_x = grid_x0 + radius * cell;
        double center_y = radius * cell;
        cairo_set_source_rgba(picker_cr, 1, 1, 1, 0.9);
        cairo_set_line_width(picker_cr, 2.0);
        cairo_rectangle(picker_cr, center_x + 1, center_y + 1, cell - 2, cell - 2);
        cairo_stroke(picker_cr);

        // Draw hex label below the grid
        double lbl_x = (surf_w - label_w) / 2.0;
        double lbl_y = grid_px + 4;

        // Rounded-rect background
        double lbl_radius = 4.0;
        double lx = lbl_x + 0.5, ly = lbl_y + 0.5;
        double lw = label_w - 1.0, lh = label_h - 1.0;

        cairo_new_sub_path(picker_cr);
        cairo_arc(picker_cr, lx + lw - lbl_radius, ly + lbl_radius, lbl_radius, -M_PI / 2, 0);
        cairo_arc(picker_cr, lx + lw - lbl_radius, ly + lh - lbl_radius, lbl_radius, 0, M_PI / 2);
        cairo_arc(picker_cr, lx + lbl_radius, ly + lh - lbl_radius, lbl_radius, M_PI / 2, M_PI);
        cairo_arc(picker_cr, lx + lbl_radius, ly + lbl_radius, lbl_radius, M_PI, 3 * M_PI / 2);
        cairo_close_path(picker_cr);

        cairo_set_source_rgba(picker_cr, 0, 0, 0, 0.85);
        cairo_fill(picker_cr);

        // Draw color text
        cairo_select_font_face(picker_cr, "sans-serif",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(picker_cr, font_size);
        cairo_set_source_rgba(picker_cr, 1, 1, 1, 1);
        cairo_move_to(picker_cr,
            lbl_x + label_pad - text_ext.x_bearing,
            lbl_y + label_pad - text_ext.y_bearing);
        cairo_show_text(picker_cr, color_label.c_str());

        cairo_surface_flush(picker_surface);

        // Upload to GPU
        picker_tex = wf::owned_texture_t{picker_surface};
    }

    void compute_loupe_position(wf::pointf_t cursor)
    {
        int radius = picker_loupe_radius;
        int cell   = picker_cell_size;
        int grid   = 2 * radius + 1;
        int grid_px = grid * cell;

        // Estimate surface size (must match render_loupe)
        int surf_w = grid_px; // approximate, label may be wider but grid is dominant
        int surf_h = grid_px + 4 + 28; // approximate label height

        auto og = output->get_relative_geometry();
        double offset = 20;

        double lx = cursor.x + offset;
        double ly = cursor.y - surf_h / 2.0;

        // Flip left if too close to right edge
        if (lx + surf_w > og.x + og.width)
        {
            lx = cursor.x - offset - surf_w;
        }

        // Clamp vertically
        if (ly < og.y)
        {
            ly = og.y;
        }

        if (ly + surf_h > og.y + og.height)
        {
            ly = og.y + og.height - surf_h;
        }

        loupe_geom = {lx, ly, (double)surf_w, (double)surf_h};
    }

    void cleanup_picker()
    {
        if (picker_active)
        {
            output->render->damage(loupe_geom);
            output->render->set_redraw_always(false);
        }

        picker_active = false;
        picker_needs_read = false;
        picker_pixel_data.clear();

        if (picker_cr)
        {
            cairo_destroy(picker_cr);
            picker_cr = nullptr;
        }

        if (picker_surface)
        {
            cairo_surface_destroy(picker_surface);
            picker_surface = nullptr;
        }

        picker_tex = wf::owned_texture_t{};
        loupe_geom = {0, 0, 0, 0};
        prev_loupe_geom = {0, 0, 0, 0};

        output->render->damage_whole();
    }

    wf::effect_hook_t frame_pre_paint = [=] ()
    {
        wf::pointf_t cursor_pos;

        if (is_frozen)
        {
            cursor_pos = frozen_position;
        }
        else
        {
            cursor_pos = output->get_cursor_position();
        }

        auto og = output->get_relative_geometry();
        double half_width = line_width * 0.5;

        /* Damage last frame geometry to clear it */
        output->render->damage(geometry[0]);
        output->render->damage(geometry[1]);

        /* All geometry is in logical (output-local) coordinates.
         * The render pipeline handles scaling to physical pixels internally. */
        geometry[0] = wf::geometry_t{
            cursor_pos.x - half_width,
            og.y,
            (double)line_width,
            og.height
        };

        geometry[1] = wf::geometry_t{
            og.x,
            cursor_pos.y - half_width,
            og.width,
            (double)line_width
        };

        output->render->damage(geometry[0]);
        output->render->damage(geometry[1]);

        // Update measurement if actively tracking cursor
        if (measure_state == MEASURE_ACTIVE)
        {
            auto cur_b = cursor_pos;
            if (cur_b.x != last_measure_b.x || cur_b.y != last_measure_b.y)
            {
                damage_measure(); // damage old position
                measure_point_b = cur_b;
                last_measure_b  = cur_b;
                compute_measure_rect();
                render_measure_label();
                damage_measure(); // damage new position
            }
        }

        // Update picker loupe position
        if (picker_active)
        {
            // Damage old loupe position
            output->render->damage(prev_loupe_geom);
            output->render->damage(loupe_geom);

            compute_loupe_position(cursor_pos);
            picker_needs_read = true;

            // Damage new loupe position
            output->render->damage(loupe_geom);
            prev_loupe_geom = loupe_geom;
        }
    };

    wf::effect_hook_t post_hook = [=] ()
    {
        auto target_fb = output->render->get_target_framebuffer();
        wf::pointf_t coords;

        if (is_frozen)
        {
            coords = frozen_position;
        }
        else
        {
            coords = wf::get_core().get_cursor_position();
        }

        // Only render if cursor is on this output (or if frozen)
        if (!is_frozen && !(output->get_layout_geometry() & coords))
        {
            return;
        }

        wf::regionf_t region;
        region |= geometry[0];
        region |= geometry[1];
        /* Note: we intentionally do NOT intersect with get_swap_damage() here.
         * get_swap_damage() returns buffer/physical coordinates, while our
         * geometry is in logical coordinates. The coordinate space mismatch
         * causes empty intersections on scaled outputs, making lines invisible.
         * add_rect() handles clipping internally. */

        auto base_alpha = wf::color_t(line_color).a;
        auto r = wf::color_t(line_color).r;
        auto g = wf::color_t(line_color).g;
        auto b = wf::color_t(line_color).b;

        // Fade crosshair while actively measuring
        double crosshair_alpha = base_alpha;
        if (measure_state == MEASURE_ACTIVE)
        {
            crosshair_alpha *= crosshair_measure_opacity / 100.0;
        }

        auto pass = output->render->get_current_pass();

        // Hide crosshairs entirely while picking to avoid tainting pixel reads
        if (!picker_active)
        {
            wf::color_t crosshair_color = wf::color_t{
                r * crosshair_alpha,
                g * crosshair_alpha,
                b * crosshair_alpha,
                crosshair_alpha
            };

            pass->add_rect(crosshair_color, target_fb, target_fb.geometry, region);
        }

        // Draw measurement overlay
        if (measure_state != MEASURE_IDLE)
        {
            auto ma = wf::color_t(measure_color_opt).a;
            wf::color_t measure_color = wf::color_t{
                wf::color_t(measure_color_opt).r * ma,
                wf::color_t(measure_color_opt).g * ma,
                wf::color_t(measure_color_opt).b * ma,
                ma
            };

            wf::regionf_t measure_region;
            for (int i = 0; i < 4; i++)
            {
                measure_region |= measure_rect_geom[i];
            }

            pass->add_rect(measure_color, target_fb, target_fb.geometry, measure_region);

            if (measure_label_geom.width > 0 && measure_label_geom.height > 0)
            {
                pass->add_texture(measure_tex.get_texture(), target_fb,
                    measure_label_geom, measure_label_geom);
            }
        }

        // Draw color picker loupe
        if (picker_active && picker_needs_read)
        {
            picker_needs_read = false;

            int radius = picker_loupe_radius;
            int grid   = 2 * radius + 1;

            // Use output-local cursor position (not global coords).
            // Anchor the grid on the logical pixel the cursor sits in, so the
            // sampled cells line up with pixel boundaries rather than the
            // sub-pixel cursor position.
            auto local_cursor = output->get_cursor_position();
            double local_x = std::floor(local_cursor.x);
            double local_y = std::floor(local_cursor.y);

            // Read pixels from framebuffer using custom_gles_subpass
            pass->custom_gles_subpass([&]
            {
                auto fb_size = target_fb.get_size();
                int fb_w = fb_size.width;
                int fb_h = fb_size.height;

                // Convert output-local logical coords to physical framebuffer coords
                // using framebuffer_box_from_geometry_box (handles scale + transform)
                wf::geometry_t cursor_box = {
                    local_x - radius,
                    local_y - radius,
                    (double)grid,
                    (double)grid
                };
                auto fb_box = target_fb.framebuffer_box_from_geometry_box(cursor_box);

                // Clamp to framebuffer bounds
                int clamped_x = std::max(0, fb_box.x);
                int clamped_y = std::max(0, fb_box.y);
                int clamped_w = std::min(fb_box.x + fb_box.width, fb_w) - clamped_x;
                int clamped_h = std::min(fb_box.y + fb_box.height, fb_h) - clamped_y;

                if (clamped_w <= 0 || clamped_h <= 0)
                {
                    return;
                }

                // wlroots GLES2 uses FLIPPED_180 projection for buffer passes,
                // so buffer Y maps directly to GL Y (no Y-flip needed).
                // Data comes back in top-to-bottom order (no row reversal needed).
                std::vector<uint8_t> pixels(clamped_w * clamped_h * 4);
                GL_CALL(glReadPixels(clamped_x, clamped_y, clamped_w, clamped_h,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));

                // Sample grid×grid logical pixels from the physical-resolution read
                picker_pixel_data.resize(grid * grid * 4, 0);

                float scale_x = (float)fb_box.width / grid;
                float scale_y = (float)fb_box.height / grid;
                int offset_x = clamped_x - fb_box.x;
                int offset_y = clamped_y - fb_box.y;

                for (int gy = 0; gy < grid; gy++)
                {
                    for (int gx = 0; gx < grid; gx++)
                    {
                        // Physical pixel for this grid cell (center of cell)
                        int px = (int)(gx * scale_x + scale_x / 2) - offset_x;
                        int py = (int)(gy * scale_y + scale_y / 2) - offset_y;

                        int dst = (gy * grid + gx) * 4;
                        if (px >= 0 && px < clamped_w && py >= 0 && py < clamped_h)
                        {
                            int src = (py * clamped_w + px) * 4;
                            picker_pixel_data[dst + 0] = pixels[src + 0];
                            picker_pixel_data[dst + 1] = pixels[src + 1];
                            picker_pixel_data[dst + 2] = pixels[src + 2];
                            picker_pixel_data[dst + 3] = pixels[src + 3];
                        }
                        else
                        {
                            picker_pixel_data[dst + 0] = 0;
                            picker_pixel_data[dst + 1] = 0;
                            picker_pixel_data[dst + 2] = 0;
                            picker_pixel_data[dst + 3] = 255;
                        }
                    }
                }

                // Extract center pixel color
                int center_idx = (radius * grid + radius) * 4;
                picked_color[0] = picker_pixel_data[center_idx + 0];
                picked_color[1] = picker_pixel_data[center_idx + 1];
                picked_color[2] = picker_pixel_data[center_idx + 2];
                picked_color[3] = picker_pixel_data[center_idx + 3];
            });

            // Render the loupe from captured pixel data
            render_loupe();

            if (loupe_geom.width > 0 && loupe_geom.height > 0)
            {
                pass->add_texture(picker_tex.get_texture(), target_fb,
                    loupe_geom, loupe_geom);
            }
        }
        else if (picker_active && loupe_geom.width > 0 && loupe_geom.height > 0)
        {
            // Loupe already rendered, just re-draw it
            pass->add_texture(picker_tex.get_texture(), target_fb,
                loupe_geom, loupe_geom);
        }
    };

    void fini() override
    {
        if (is_active)
        {
            cleanup_measure();
            cleanup_picker();
            output->render->rem_effect(&post_hook);
            output->render->rem_effect(&frame_pre_paint);
            output->render->damage_whole();
        }
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_screen_tools>);
