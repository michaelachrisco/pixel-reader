#include "./token_view.h"

#include "./token_line_scroller.h"
#include "./token_view_styling.h"

#include "epub/epub_reader.h"
#include "reader/system_styling.h"
#include "sys/keymap.h"
#include "sys/screen.h"
#include "util/sdl_font_cache.h"
#include "util/sdl_utils.h"
#include "util/throttled.h"

#include <stdexcept>
#include <iostream>

// Explore enough lines ahead to detect end of book before we get there
#define NUM_PREFETCH_LINES 30

namespace {

bool line_fits_on_screen(TTF_Font *font, const char *s, uint32_t len)
{
    int w = SCREEN_WIDTH, h;

    char *mut_s = (char*)s;
    char replaced = mut_s[len];
    mut_s[len] = 0;

    TTF_SizeUTF8(font, mut_s, &w, &h);
    mut_s[len] = replaced;

    return w <= SCREEN_WIDTH;
}

}  // namespace

struct TokenViewState
{
    SystemStyling &sys_styling;
    TokenViewStyling &token_view_styling;
    const uint32_t sys_styling_sub_id;
    const uint32_t token_view_styling_sub_id;

    TTF_Font *current_font = nullptr;

    const int line_padding = 4;
    int line_height;

    TokenLineScroller line_scroller;

    bool needs_render = true;

    std::string title;
    int title_progress_percent = 0;

    std::function<void(DocAddr)> on_scroll;

    Throttled line_scroll_throttle;
    Throttled page_scroll_throttle;

    int num_text_display_lines() const
    {
        bool show_title_bar = token_view_styling.get_show_title_bar();
        int num_display_lines = (SCREEN_HEIGHT + line_padding) / line_height;
        if (num_display_lines > NUM_PREFETCH_LINES)
        {
            throw std::runtime_error("num_display_lines > NUM_PREFETCH_LINES");
        }
        return num_display_lines - (show_title_bar ? 1 : 0);
    }

    Uint16 content_crop_bottom() const
    {
        if (!token_view_styling.get_show_title_bar())
        {
            return SCREEN_HEIGHT;
        }
        return num_text_display_lines() * line_height;
    }

    // Adjust scroll amount to avoid going beyond start or end of book.
    int get_bounded_scroll_amount(int num_lines) const
    {
        int cur_line = line_scroller.get_line_number();
        int new_line = cur_line + num_lines;
        auto first_line = line_scroller.first_line_number();
        auto last_line = line_scroller.last_line_number();

        if (last_line)
        {
            new_line = std::min(
                *last_line - num_text_display_lines(),
                new_line
            );
        }
        if (first_line)
        {
            new_line = std::max(
                *first_line,
                new_line
            );
        }

        return new_line - cur_line;
    }

    TokenViewState(EPubReader &reader, DocAddr address, SystemStyling &sys_styling, TokenViewStyling &token_view_styling)
        : sys_styling(sys_styling),
          token_view_styling(token_view_styling),
          sys_styling_sub_id(sys_styling.subscribe_to_changes([this]() {
              // Color theme and font size
              current_font = cached_load_font(
                  this->sys_styling.get_font_name(),
                  this->sys_styling.get_font_size()
              );
              line_height = detect_line_height(current_font) + line_padding;
              line_scroller.set_line_height_pixels(line_height);
              line_scroller.reset_buffer();  // need to re-wrap lines if font-size changed
              needs_render = true;
          })),
          token_view_styling_sub_id(token_view_styling.subscribe_to_changes([this]() {
              needs_render = true;
          })),
          current_font(cached_load_font(
              sys_styling.get_font_name(),
              sys_styling.get_font_size()
          )),
          line_height(detect_line_height(sys_styling.get_font_name(), sys_styling.get_font_size()) + line_padding),
          line_scroller(
              reader,
              address,
              NUM_PREFETCH_LINES,
              [this](const char *s, uint32_t len) {
                  return line_fits_on_screen(current_font, s, len);
              },
              line_height
          ),
          line_scroll_throttle(250, 50),
          page_scroll_throttle(750, 150)
    {
    }

    ~TokenViewState()
    {
        sys_styling.unsubscribe_from_changes(sys_styling_sub_id);
        token_view_styling.unsubscribe_from_changes(token_view_styling_sub_id);
    }
};

TokenView::TokenView(EPubReader &reader, DocAddr address, SystemStyling &sys_styling, TokenViewStyling &token_view_styling)
    : state(std::make_unique<TokenViewState>(reader, address, sys_styling, token_view_styling))
{
}

TokenView::~TokenView()
{
}

bool TokenView::render(SDL_Surface *dest_surface, bool force_render)
{
    if (!state->needs_render && !force_render)
    {
        return false;
    }
    state->needs_render = false;

    scroll(0);  // Will adjust scroll position for end of book

    TTF_Font *font = state->current_font;
    const auto &theme = state->sys_styling.get_loaded_color_theme();
    const int line_height = state->line_height;
    const int line_padding = state->line_padding;

    // Clear screen
    {
        SDL_Rect rect = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
        const auto &bgcolor = theme.background;

        SDL_FillRect(
            dest_surface,
            &rect,
            SDL_MapRGB(dest_surface->format, bgcolor.r, bgcolor.g, bgcolor.b)
        );
    }

    Sint16 y = 0;

    int num_text_display_lines = state->num_text_display_lines();

    for (int i = 0; i < num_text_display_lines; ++i)
    {
        const DisplayLine *line = state->line_scroller.get_line_relative(i);
        if (line)
        {
            SDL_Rect dest_rect = {0, y, 0, 0};
            if (line->type == DisplayLine::Type::Text)
            {
                const char *s = static_cast<const TextLine *>(line)->text.c_str();
                SDL_Surface *surface = TTF_RenderUTF8_Shaded(font, s, theme.main_text, theme.background);
                SDL_BlitSurface(surface, nullptr, dest_surface, &dest_rect);
                SDL_FreeSurface(surface);
            }
            else if (line->type == DisplayLine::Type::Image || (line->type == DisplayLine::Type::ImageRef && i == 0))
            {
                const ImageLine *image_line = nullptr;
                uint32_t line_offset = 0;

                if (line->type == DisplayLine::Type::ImageRef)
                {
                    line_offset = static_cast<const ImageRefLine *>(line)->offset;
                    const DisplayLine *ref_line = state->line_scroller.get_line_relative(i - line_offset);
                    if (ref_line)
                    {
                        if (ref_line->type != DisplayLine::Type::Image)
                        {
                            throw std::runtime_error("ImageRefLine points to non image");
                        }
                        image_line = static_cast<const ImageLine *>(ref_line);
                    }
                }
                else
                {
                    image_line = static_cast<const ImageLine *>(line);
                }

                if (image_line)
                {
                    auto *surface = state->line_scroller.load_scaled_image(image_line->image_path);

                    uint32_t y_center = (image_line->num_lines * line_height - image_line->height) / 2;
                    Sint16 crop_y = line_offset * line_height - y_center;
                    if (surface && crop_y < (Sint16)image_line->height)
                    {
                        Uint16 width = image_line->width;
                        Uint16 height = image_line->height - crop_y;
                        auto y_bottom = y + height;

                        Uint16 y_limit = state->content_crop_bottom();
                        if (y_bottom > y_limit)
                        {
                            height -= y_bottom - y_limit;
                        }

                        dest_rect.x = (SCREEN_WIDTH - width) / 2;
                        SDL_Rect src_rect = {0, crop_y, width, height};
                        SDL_BlitSurface(surface, &src_rect, dest_surface, &dest_rect);
                    }
                }
            }
        }

        y += line_height;
    }

    if (state->token_view_styling.get_show_title_bar())
    {
        y = line_height * num_text_display_lines;

        SDL_Rect dest_rect = {0, y, 0, 0};
        SDL_Rect title_crop_rect = {0, 0, 0, (Uint16)line_height};

        {
            char page_number[32];
            snprintf(page_number, sizeof(page_number), " %d%%", state->title_progress_percent);

            SDL_Surface *page_surface = TTF_RenderUTF8_Shaded(font, page_number, theme.secondary_text, theme.background);

            dest_rect.x = SCREEN_WIDTH - page_surface->w - line_padding;
            title_crop_rect.w = SCREEN_WIDTH - line_padding * 2 - page_surface->w;

            SDL_BlitSurface(page_surface, nullptr, dest_surface, &dest_rect);
            SDL_FreeSurface(page_surface);
        }

        if (state->title.size() > 0)
        {
            dest_rect.x = 0;
            SDL_Surface *surface = TTF_RenderUTF8_Shaded(font, state->title.c_str(), theme.secondary_text, theme.background);
            SDL_BlitSurface(surface, &title_crop_rect, dest_surface, &dest_rect);
            SDL_FreeSurface(surface);
        }
    }

    return true;
}

void TokenView::scroll(int num_lines)
{
    num_lines = state->get_bounded_scroll_amount(num_lines);
    if (num_lines != 0)
    {
        state->needs_render = true;
        state->line_scroller.seek_lines_relative(num_lines);
        if (state->on_scroll)
        {
            state->on_scroll(get_address());
        }
    }
}

void TokenView::on_keypress(SDLKey key)
{
    switch (key) {
        case SW_BTN_UP:
            scroll(-1);
            break;
        case SW_BTN_DOWN:
            scroll(1);
            break;
        case SW_BTN_LEFT:
        case SW_BTN_L1:
            scroll(-state->num_text_display_lines());
            break;
        case SW_BTN_RIGHT:
        case SW_BTN_R1:
            scroll(state->num_text_display_lines());
            break;
        default:
            break;
    }
}

void TokenView::on_keyheld(SDLKey key, uint32_t held_time_ms)
{
    switch (key) {
        case SW_BTN_UP:
        case SW_BTN_DOWN:
            if (state->line_scroll_throttle(held_time_ms))
            {
                on_keypress(key);
            }
            break;
        case SW_BTN_LEFT:
        case SW_BTN_RIGHT:
        case SW_BTN_L1:
        case SW_BTN_R1:
            if (state->page_scroll_throttle(held_time_ms))
            {
                on_keypress(key);
            }
            break;
        default:
            break;
    }
}

bool TokenView::is_done()
{
    return false;
}

DocAddr TokenView::get_address() const
{
    const DisplayLine *line = state->line_scroller.get_line_relative(0);
    if (line)
    {
        return line->address;
    }
    return make_address();
}

void TokenView::seek_to_address(DocAddr address)
{
    state->line_scroller.seek_to_address(address);
    state->needs_render = true;
}

void TokenView::set_title(const std::string &title)
{
    if (title != state->title)
    {
        state->title = title;
        state->needs_render = true;
    }
}

void TokenView::set_title_progress(int percent)
{
    if (percent != state->title_progress_percent)
    {
        state->title_progress_percent = percent;
        state->needs_render = true;
    }
}

void TokenView::set_on_scroll(std::function<void(DocAddr)> callback)
{
    state->on_scroll = callback;
}
