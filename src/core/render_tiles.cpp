#include "core/render_tiles.h"
#include "core/colour.h"
#include "core/firefly_filter.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>
#ifdef HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

namespace dusk_tiles {
namespace {
void hilbert_point(int n, int d, int& x, int& y) {
    x = y = 0;
    for (int s = 1; s < n; s *= 2) {
        int rx = 1 & (d / 2), ry = 1 & (d ^ rx);
        if (ry == 0) {
            if (rx == 1) {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            std::swap(x, y);
        }
        x += s * rx;
        y += s * ry;
        d /= 4;
    }
}
} // namespace

std::vector<rect> make_tiles(int width, int height, int size, int order) {
    std::vector<rect> tiles;
    if (width <= 0 || height <= 0 || size <= 0) return tiles;
    const int columns = (width + size - 1) / size, rows = (height + size - 1) / size;
    tiles.reserve(size_t(columns) * rows);
    auto add = [&](int column, int row) {
        if (column < 0 || row < 0 || column >= columns || row >= rows) return;
        tiles.push_back({column * size, row * size, std::min(width, (column + 1) * size),
                         std::min(height, (row + 1) * size)});
    };
    if (order == 1) {
        int n = 1;
        while (n < std::max(columns, rows)) n *= 2;
        for (int d = 0; d < n * n; ++d) {
            int x, y;
            hilbert_point(n, d, x, y);
            add(x, y);
        }
    } else if (order == 0) {
        // Walk right, down, left, up with run lengths 1,1,2,2,3,3... from
        // the centre bucket, keeping the positions inside the grid.
        int x = (columns - 1) / 2, y = (rows - 1) / 2;
        add(x, y);
        const int dx[] = {1, 0, -1, 0}, dy[] = {0, 1, 0, -1};
        for (int run = 1, direction = 0; int(tiles.size()) < columns * rows; ++run) {
            for (int repeat = 0; repeat < 2; ++repeat, direction = (direction + 1) % 4) {
                for (int step = 0; step < run; ++step) {
                    x += dx[direction];
                    y += dy[direction];
                    add(x, y);
                }
            }
        }
    } else {
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < columns; ++column) add(column, row);
    }
    return tiles;
}

void encode_display(const float* linear, double exposure, std::uint8_t* out) {
    double rgb[3];
    for (int c = 0; c < 3; ++c) rgb[c] = std::isfinite(linear[c]) ? std::max(0.0, double(linear[c])) * exposure : 0;
    double lum = .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2], scale = lum < 1e-6 ? 0 : 1 / (1 + lum);
    for (int c = 0; c < 3; ++c)
        out[c] = std::uint8_t(256 * std::clamp(linear_to_gamma(rgb[c] * scale), 0.0, .999));
}

struct denoiser::impl {
    int width, height, tile, columns, rows, window_width, window_height, margin;
    const float *color, *albedo, *normal, *indirect;
    float strength;
    double exposure;
    int threads;
    render_progress_state* progress;
    const std::atomic<bool>* cancel;

    std::vector<std::uint8_t> done, queued; // guarded by mutex
    std::unique_ptr<std::atomic<std::uint8_t>[]> ready;
    std::vector<std::uint8_t> encoded;
    std::atomic<int> count{0};
    std::deque<int> queue;
    int busy = 0;
    bool stopping = false;
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::thread worker;

    rect bounds(int index) const {
        int column = index % columns, row = index / columns;
        return {column * tile, row * tile, std::min(width, (column + 1) * tile), std::min(height, (row + 1) * tile)};
    }
    // A fixed-size window keeps one committed OIDN filter for every bucket;
    // at image borders it shifts inward instead of shrinking.
    rect window(int index) const {
        rect r = bounds(index);
        int x = std::clamp(r.x0 - margin, 0, width - window_width);
        int y = std::clamp(r.y0 - margin, 0, height - window_height);
        return {x, y, x + window_width, y + window_height};
    }
    bool covered(int index) const {
        rect w = window(index);
        for (int row = w.y0 / tile; row <= (w.y1 - 1) / tile; ++row)
            for (int column = w.x0 / tile; column <= (w.x1 - 1) / tile; ++column)
                if (!done[size_t(row) * columns + column]) return false;
        return true;
    }

    void run() {
#ifdef HAVE_OIDN
        const size_t values = size_t(window_width) * window_height * 3;
        std::vector<float> in_color(values), in_albedo(values), in_normal(values), in_indirect, filtered(values);
        if (indirect) in_indirect.resize(values);
        oidn::DeviceRef device;
        oidn::FilterRef filter;
        bool usable = false;
        try {
            device = oidn::newDevice(oidn::DeviceType::CPU);
            if (threads > 0) device.set("numThreads", threads);
            device.commit();
            filter = device.newFilter("RT");
            filter.setImage("color", in_color.data(), oidn::Format::Float3, window_width, window_height);
            filter.setImage("albedo", in_albedo.data(), oidn::Format::Float3, window_width, window_height);
            filter.setImage("normal", in_normal.data(), oidn::Format::Float3, window_width, window_height);
            filter.setImage("output", filtered.data(), oidn::Format::Float3, window_width, window_height);
            filter.set("hdr", true);
            filter.set("srgb", false);
            filter.set("cleanAux", false);
            filter.commit();
            const char* message = nullptr;
            usable = device.getError(message) == oidn::Error::None;
        } catch (const std::exception&) {
            usable = false;
        }
#endif
        for (;;) {
            int index;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                index = queue.front();
                queue.pop_front();
                ++busy;
            }
#ifdef HAVE_OIDN
            if (usable && !(cancel && cancel->load())) {
                const rect r = bounds(index), w = window(index);
                render_tile_activity activity{r.x0, r.y0, r.width(), r.height(), render_tile_activity::denoising};
                if (progress) progress->begin_tile(activity);
                for (int y = 0; y < window_height; ++y) {
                    const size_t from = (size_t(w.y0 + y) * width + w.x0) * 3, to = size_t(y) * window_width * 3,
                                 bytes = size_t(window_width) * 3 * sizeof(float);
                    std::memcpy(&in_color[to], color + from, bytes);
                    std::memcpy(&in_albedo[to], albedo + from, bytes);
                    std::memcpy(&in_normal[to], normal + from, bytes);
                    if (indirect) std::memcpy(&in_indirect[to], indirect + from, bytes);
                }
                if (indirect) dusk_fireflies::reconstruct(in_color, in_indirect, window_width, window_height, 1);
                filter.execute();
                const char* message = nullptr;
                bool ok = device.getError(message) == oidn::Error::None;
                for (int y = r.y0; ok && y < r.y1; ++y) {
                    for (int x = r.x0; x < r.x1; ++x) {
                        size_t local = (size_t(y - w.y0) * window_width + (x - w.x0)) * 3, global = (size_t(y) * width + x) * 3;
                        float blended[3];
                        for (int c = 0; c < 3; ++c) {
                            float raw = in_color[local + c], value = filtered[local + c];
                            value = std::isfinite(value) ? std::max(0.f, value) : raw;
                            blended[c] = raw + strength * (value - raw);
                        }
                        encode_display(blended, exposure, &encoded[global]);
                    }
                }
                if (ok) {
                    ready[index].store(1, std::memory_order_release);
                    ++count;
                }
                if (progress) progress->end_tile(activity);
            }
#endif
            {
                std::lock_guard<std::mutex> lock(mutex);
                --busy;
            }
            idle.notify_all();
        }
    }
};

denoiser::denoiser(int width, int height, int tile_size, const float* color, const float* albedo,
                   const float* normal, const float* indirect, double strength, double exposure, int threads,
                   render_progress_state* progress, const std::atomic<bool>* cancel)
    : self(std::make_unique<impl>()) {
    auto& s = *self;
    s.width = width;
    s.height = height;
    s.tile = std::max(1, tile_size);
    s.columns = (width + s.tile - 1) / s.tile;
    s.rows = (height + s.tile - 1) / s.tile;
    // Half a bucket (at most 32 pixels) of real context on each side keeps
    // every window within its bucket's immediate neighbours.
    s.margin = std::min(32, s.tile / 2);
    s.window_width = std::min(width, s.tile + 2 * s.margin);
    s.window_height = std::min(height, s.tile + 2 * s.margin);
    s.color = color;
    s.albedo = albedo;
    s.normal = normal;
    s.indirect = indirect;
    s.strength = float(std::clamp(strength, 0.0, 1.0));
    s.exposure = exposure;
    s.threads = threads;
    s.progress = progress;
    s.cancel = cancel;
    const size_t tiles = size_t(s.columns) * s.rows;
    s.done.assign(tiles, 0);
    s.queued.assign(tiles, 0);
    s.ready = std::make_unique<std::atomic<std::uint8_t>[]>(tiles);
    for (size_t i = 0; i < tiles; ++i) s.ready[i].store(0);
    s.encoded.assign(size_t(width) * height * 3, 0);
#ifdef HAVE_OIDN
    if (width > 0 && height > 0 && color && albedo && normal) s.worker = std::thread([this] { self->run(); });
#endif
}

denoiser::~denoiser() { stop(); }

void denoiser::finished(const rect& tile) {
    auto& s = *self;
    if (!s.worker.joinable() || tile.width() <= 0 || tile.height() <= 0) return;
    const int column = tile.x0 / s.tile, row = tile.y0 / s.tile;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.done[size_t(row) * s.columns + column] = 1;
        // Windows reach at most one bucket past their own, or two where an
        // edge window shifts inward, so only nearby buckets can become ready.
        for (int r = std::max(0, row - 2); r <= std::min(s.rows - 1, row + 2); ++r) {
            for (int c = std::max(0, column - 2); c <= std::min(s.columns - 1, column + 2); ++c) {
                const int index = r * s.columns + c;
                if (s.done[index] && !s.queued[index] && s.covered(index)) {
                    s.queued[index] = 1;
                    s.queue.push_back(index);
                }
            }
        }
    }
    s.wake.notify_one();
}

void denoiser::overlay(std::vector<std::uint8_t>& rgb) const {
    const auto& s = *self;
    if (rgb.size() != s.encoded.size()) return;
    for (int index = 0; index < s.columns * s.rows; ++index) {
        if (!s.ready[index].load(std::memory_order_acquire)) continue;
        const rect r = s.bounds(index);
        for (int y = r.y0; y < r.y1; ++y) {
            const size_t p = (size_t(y) * s.width + r.x0) * 3;
            std::memcpy(&rgb[p], &s.encoded[p], size_t(r.width()) * 3);
        }
    }
}

int denoiser::denoised() const { return self->count.load(); }

void denoiser::wait_idle() {
    auto& s = *self;
    std::unique_lock<std::mutex> lock(s.mutex);
    s.idle.wait(lock, [&] { return !s.worker.joinable() || (s.queue.empty() && s.busy == 0); });
}

void denoiser::stop() {
    auto& s = *self;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stopping = true;
        s.queue.clear();
    }
    s.wake.notify_all();
    if (s.worker.joinable()) s.worker.join();
    s.idle.notify_all();
}
} // namespace dusk_tiles
