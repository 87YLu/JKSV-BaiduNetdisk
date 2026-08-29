#include "appstates/TaskState.hpp"

#include "appstates/FadeState.hpp"
#include "graphics/colors.hpp"
#include "graphics/screen.hpp"
#include "input.hpp"
#include "sdl.hpp"
#include "strings/strings.hpp"
#include "ui/PopMessageManager.hpp"

//                      ---- Construction ----

TaskState::TaskState(sys::threadpool::JobFunction function, sys::Task::TaskData taskData)
{
    m_task = std::make_unique<sys::Task>(function, taskData);
}

//                      ---- Public functions ----

void TaskState::update()
{
    BaseTask::pop_on_plus();
    BaseTask::update_loading_glyph();

    if (!m_taskImage)
    {
        std::vector<uint8_t> image = m_task->take_image();
        if (!image.empty())
        {
            auto texture = std::make_shared<sdl::Texture>(image.data(), image.size());
            if (texture->get_width() > 0 && texture->get_height() > 0) { m_taskImage = std::move(texture); }
        }
    }

    if (!m_task->is_running()) { TaskState::deactivate_state(); }
}

void TaskState::render()
{
    const std::string status = m_task->get_status();

    sdl::render_rect_fill(sdl::Texture::Null, 0, 0, graphics::SCREEN_WIDTH, graphics::SCREEN_HEIGHT, colors::DIM_BACKGROUND);

    if (m_taskImage)
    {
        static constexpr int QR_X          = 120;
        static constexpr int QR_Y          = 150;
        static constexpr int QR_SIZE       = 380;
        static constexpr int QR_QUIET_ZONE = 12;
        static constexpr int TEXT_X        = 560;
        static constexpr int TEXT_Y        = 225;
        static constexpr int TEXT_WIDTH    = 600;

        sdl::render_rect_fill(sdl::Texture::Null,
                              QR_X - QR_QUIET_ZONE,
                              QR_Y - QR_QUIET_ZONE,
                              QR_SIZE + (QR_QUIET_ZONE * 2),
                              QR_SIZE + (QR_QUIET_ZONE * 2),
                              colors::WHITE);
        m_taskImage->render_stretched(sdl::Texture::Null, QR_X, QR_Y, QR_SIZE, QR_SIZE);
        sdl::text::render(
            sdl::Texture::Null, TEXT_X, TEXT_Y, BaseTask::FONT_SIZE, TEXT_WIDTH, colors::WHITE, status);
    }
    else
    {
        const int statusX = 640 - (sdl::text::get_width(BaseTask::FONT_SIZE, status.c_str()) / 2);
        sdl::text::render(sdl::Texture::Null, statusX, 351, BaseTask::FONT_SIZE, sdl::text::NO_WRAP, colors::WHITE, status);
    }

    BaseTask::render_loading_glyph();
}

void TaskState::deactivate_state()
{
    FadeState::create_and_push(colors::DIM_BACKGROUND, colors::ALPHA_FADE_END, colors::ALPHA_FADE_BEGIN, nullptr);
    BaseState::deactivate();
}
