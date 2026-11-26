#include <d3d12.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace reshade;
namespace a = reshade::api;

constexpr unsigned interval_count = 4;
constexpr size_t max_list_rows = 24000;
constexpr size_t max_trace_rows = 120000;
constexpr size_t max_live_rows = 120000;
constexpr size_t max_descriptor_slots = 524288;
constexpr uint32_t max_range_slots = 64;
constexpr GUID heap_id_guid{0x31d39db3, 0x725b, 0x467c, {0xae, 0x93, 0x12, 0x2c, 0x3d, 0x87, 0x95, 0x31}};

struct Texture {
    uint64_t id = 0;
    a::resource_desc desc;
};

struct Descriptor {
    uint64_t resource = 0;
    uint64_t id = 0;
    a::format format = a::format::unknown;
    bool uav = false;
};

struct Pipeline {
    uint64_t pixel_shader = 0;
    uint64_t compute_shader = 0;
};

struct Row {
    std::string text;
    uint32_t repeat = 1;
};

struct List {
    uint64_t pipeline = 0;
    uint64_t graphics_layout = 0;
    uint64_t compute_layout = 0;
    size_t dropped = 0;
    std::vector<Row> rows;
};

// ZZZ 3.2 produced this sequence in all six rendered frames of the supplied traces.
constexpr uint64_t half_mask_shader = 10793278501038871532ull;
constexpr uint64_t blur_mask_shader = 18023612121467718113ull;
constexpr uint64_t upsample_mask_shader = 9317972913748041275ull;
constexpr uint32_t probe_width = 1720;
constexpr uint32_t probe_height = 720;

enum class MaskOverride : uint8_t { off, white, black };
enum class MaskStage : uint8_t { waiting, half, blurred };

struct MaskList {
    uint64_t pixel_shader = 0;
    a::resource_view target{};
    a::resource target_resource{};
    a::resource half_resource{};
    a::viewport viewport{};
    MaskStage stage = MaskStage::waiting;
    bool render_pass_open = false;
    bool modified = false;
};

struct MaskProbe {
    a::swapchain *swapchain = nullptr;
    HWND window = nullptr;
    MaskOverride mode = MaskOverride::off;
    bool reported = false;
    std::unordered_map<a::command_list *, MaskList> lists;
};

struct HeapSlot {
    uint64_t heap = 0;
    uint32_t index = 0;
    uint32_t capacity = 0;

    uint64_t key(uint32_t offset = 0) const { return (heap << 32) | (index + offset); }
};

struct Capture {
    a::swapchain *swapchain = nullptr;
    a::device *device = nullptr;
    unsigned presents = 0;
    size_t dropped = 0;
    size_t unobserved_lists = 0;
    size_t live_rows = 0;
    uint64_t id = 0;
    std::unordered_map<a::command_list *, List> lists;
    std::unordered_set<uint64_t> emitted_textures;
    std::vector<std::string> rows;
};

std::atomic_bool capturing{false};
std::atomic_bool probing{false};
MaskProbe mask_probe;
std::mutex mutex;
Capture capture;
std::unordered_map<uint64_t, Texture> textures;
std::unordered_map<uint64_t, Pipeline> pipelines;
std::unordered_map<uint64_t, std::vector<std::vector<a::descriptor_range>>> layouts;
std::unordered_map<uint64_t, Descriptor> descriptors;
uint64_t next_texture_id = 0;
uint32_t next_heap_id = 0;
uint64_t next_capture_id = 0;
bool descriptor_limit_reached = false;
bool f10_down = false;

template <typename... Args>
std::string line(const char *kind, const Args &...args)
{
    std::ostringstream out;
    out << kind;
    ((out << '\t' << args), ...);
    return std::move(out).str();
}

void append(List &list, std::string row, bool group = false)
{
    if (group && !list.rows.empty() && list.rows.back().text == row) {
        ++list.rows.back().repeat;
    } else if (list.rows.size() < max_list_rows && capture.live_rows < max_live_rows) {
        list.rows.push_back({std::move(row)});
        ++capture.live_rows;
    } else {
        ++list.dropped;
    }
}

void append_trace(std::string row)
{
    if (capture.rows.size() < max_trace_rows)
        capture.rows.push_back(std::move(row));
    else
        ++capture.dropped;
}

bool dx12(a::device *device)
{
    return device->get_api() == a::device_api::d3d12;
}

template <typename Function>
void on_list(a::command_list *cmd, const Function &callback)
{
    if (!capturing.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    if (capture.device != nullptr && cmd->get_device() == capture.device)
        callback(capture.lists[cmd]);
}

template <typename Function>
void on_mask_list(a::command_list *cmd, const Function &callback)
{
    if (!probing.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    if (const auto state = mask_probe.lists.find(cmd); state != mask_probe.lists.end())
        callback(state->second);
}

bool screen_texture(const a::resource_desc &desc)
{
    return desc.type == a::resource_type::texture_2d;
}

const Texture &texture(a::device *device, a::resource resource)
{
    auto [it, inserted] = textures.try_emplace(resource.handle);
    if (inserted) {
        it->second.id = ++next_texture_id;
        it->second.desc = device->get_resource_desc(resource);
    }
    return it->second;
}

uint64_t emit_texture(const Texture &texture)
{
    const auto &d = texture.desc;
    if (!screen_texture(d))
        return 0;
    if (capture.emitted_textures.insert(texture.id).second)
        append_trace(line("texture", texture.id, d.texture.width, d.texture.height,
                          static_cast<uint32_t>(d.texture.format), d.texture.samples,
                          d.texture.levels, d.texture.depth_or_layers, static_cast<uint32_t>(d.usage)));
    return texture.id;
}

uint64_t emit_resource(a::device *device, a::resource resource)
{
    return resource.handle != 0 ? emit_texture(texture(device, resource)) : 0;
}

void init_resource(a::device *device, const a::resource_desc &desc, const a::subresource_data *,
                   a::resource_usage, a::resource resource)
{
    if (!dx12(device))
        return;
    const std::lock_guard lock(mutex);
    textures.insert_or_assign(resource.handle, Texture{++next_texture_id, desc});
}

void destroy_resource(a::device *device, a::resource resource)
{
    if (!dx12(device))
        return;
    const std::lock_guard lock(mutex);
    textures.erase(resource.handle);
}

uint64_t fingerprint(const a::shader_desc &shader)
{
    if (shader.code == nullptr || shader.code_size == 0)
        return 0;
    const auto *bytes = static_cast<const uint8_t *>(shader.code);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < shader.code_size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void init_pipeline(a::device *device, a::pipeline_layout, uint32_t count,
                   const a::pipeline_subobject *objects, a::pipeline pipeline)
{
    if (!dx12(device))
        return;
    Pipeline info;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &item = objects[i];
        if (item.count != 1 || item.data == nullptr)
            continue;
        if (item.type == a::pipeline_subobject_type::pixel_shader)
            info.pixel_shader = fingerprint(*static_cast<const a::shader_desc *>(item.data));
        if (item.type == a::pipeline_subobject_type::compute_shader)
            info.compute_shader = fingerprint(*static_cast<const a::shader_desc *>(item.data));
    }
    const std::lock_guard lock(mutex);
    pipelines.insert_or_assign(pipeline.handle, info);
}

void destroy_pipeline(a::device *device, a::pipeline pipeline)
{
    if (!dx12(device))
        return;
    const std::lock_guard lock(mutex);
    pipelines.erase(pipeline.handle);
}

void init_layout(a::device *device, uint32_t count, const a::pipeline_layout_param *params,
                 a::pipeline_layout layout)
{
    if (!dx12(device))
        return;
    std::vector<std::vector<a::descriptor_range>> tables(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (params[i].type == a::pipeline_layout_param_type::descriptor_table) {
            const auto &table = params[i].descriptor_table;
            if (table.count != 0)
                tables[i].assign(table.ranges, table.ranges + table.count);
        } else if (params[i].type == a::pipeline_layout_param_type::descriptor_table_with_static_samplers) {
            const auto &table = params[i].descriptor_table_with_static_samplers;
            for (uint32_t j = 0; j < table.count; ++j)
                tables[i].push_back(table.ranges[j]);
        }
    }
    const std::lock_guard lock(mutex);
    layouts.insert_or_assign(layout.handle, std::move(tables));
}

void destroy_layout(a::device *device, a::pipeline_layout layout)
{
    if (!dx12(device))
        return;
    const std::lock_guard lock(mutex);
    layouts.erase(layout.handle);
}

// Attach an observer ID to the heap's CPU metadata so recycled heap addresses
// cannot revive stale slots. It does not retain the heap or alter its descriptors.
HeapSlot heap_slot(a::device *device, a::descriptor_table table, uint32_t binding)
{
    if (table.handle == 0)
        return {};
    a::descriptor_heap handle{};
    uint32_t index = 0;
    device->get_descriptor_heap_offset(table, binding, 0, &handle, &index);
    if (handle.handle == 0)
        return {};
    auto *heap = reinterpret_cast<ID3D12DescriptorHeap *>(handle.handle);
    const auto desc = heap->GetDesc();
    if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || index >= desc.NumDescriptors)
        return {};
    uint32_t id = 0;
    UINT size = sizeof(id);
    if (FAILED(heap->GetPrivateData(heap_id_guid, &size, &id)) || size != sizeof(id) || id == 0) {
        id = ++next_heap_id;
        if (id == 0 || FAILED(heap->SetPrivateData(heap_id_guid, sizeof(id), &id)))
            return {};
    }
    return {id, index, desc.NumDescriptors - index};
}

bool is_uav(a::descriptor_type type)
{
    return type == a::descriptor_type::unordered_access_view ||
           type == a::descriptor_type::texture_unordered_access_view;
}

bool is_srv(a::descriptor_type type)
{
    return type == a::descriptor_type::shader_resource_view ||
           type == a::descriptor_type::texture_shader_resource_view;
}

void set_slot(uint64_t key, const Descriptor &value)
{
    if (value.id == 0) {
        descriptors.erase(key);
    } else if (descriptors.size() < max_descriptor_slots || descriptors.contains(key)) {
        descriptors.insert_or_assign(key, value);
    } else {
        descriptor_limit_reached = true;
    }
}

bool update_descriptors(a::device *device, uint32_t count, const a::descriptor_table_update *updates)
{
    if (!dx12(device))
        return false;
    const std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const auto &update = updates[i];
        if (update.type == a::descriptor_type::sampler || update.array_offset != 0)
            continue;
        const auto slot = heap_slot(device, update.table, update.binding);
        for (uint32_t j = 0; j < std::min(update.count, slot.capacity); ++j) {
            Descriptor value;
            if (is_srv(update.type) || is_uav(update.type)) {
                const auto view = static_cast<const a::resource_view *>(update.descriptors)[j];
                if (view.handle != 0) {
                    const auto res = device->get_resource_from_view(view);
                    if (res.handle != 0) {
                        const auto &target = texture(device, res);
                        // Restrict the ledger to resources relevant to frame composition.
                        constexpr auto uses = a::resource_usage::render_target |
                                              a::resource_usage::depth_stencil |
                                              a::resource_usage::unordered_access |
                                              a::resource_usage::copy_dest;
                        if (screen_texture(target.desc) && (target.desc.usage & uses) != a::resource_usage::undefined)
                            value = {res.handle, target.id, device->get_resource_view_desc(view).format,
                                     is_uav(update.type)};
                    }
                }
            }
            set_slot(slot.key(j), value);
        }
    }
    return false;
}

bool copy_descriptors(a::device *device, uint32_t count, const a::descriptor_table_copy *copies)
{
    if (!dx12(device))
        return false;
    const std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const auto &copy = copies[i];
        if (copy.source_array_offset != 0 || copy.dest_array_offset != 0)
            continue;
        const auto dest = heap_slot(device, copy.dest_table, copy.dest_binding);
        if (dest.capacity == 0)
            continue;
        const auto src = heap_slot(device, copy.source_table, copy.source_binding);
        for (uint32_t j = 0; j < std::min(copy.count, dest.capacity); ++j) {
            const auto it = j < src.capacity ? descriptors.find(src.key(j)) : descriptors.end();
            const Descriptor value = it != descriptors.end() ? it->second : Descriptor{};
            set_slot(dest.key(j), value);
        }
    }
    return false;
}

void bind_tables(a::command_list *cmd, a::shader_stage stages, a::pipeline_layout layout,
                 uint32_t first, uint32_t count, const a::descriptor_table *tables)
{
    on_list(cmd, [&](List &list) {
        if ((stages & a::shader_stage::all_graphics) != static_cast<a::shader_stage>(0))
            list.graphics_layout = layout.handle;
        if ((stages & a::shader_stage::compute) != static_cast<a::shader_stage>(0))
            list.compute_layout = layout.handle;

        const auto signature = layouts.find(layout.handle);
        for (uint32_t i = 0; i < count; ++i) {
            const auto slot = heap_slot(cmd->get_device(), tables[i], 0);
            if (!slot.capacity || signature == layouts.end() || first + i >= signature->second.size())
                continue;

            for (const auto &range : signature->second[first + i]) {
                if ((!is_srv(range.type) && !is_uav(range.type)) || range.binding >= slot.capacity)
                    continue;
                const uint32_t observed = std::min({range.count, max_range_slots, slot.capacity - range.binding});
                append(list, line("table", static_cast<uint32_t>(stages), layout.handle, first + i,
                                  slot.heap, slot.index, range.binding, range.count, observed));
                for (uint32_t j = 0; j < observed; ++j) {
                    const auto tracked = descriptors.find(slot.key(range.binding + j));
                    if (tracked == descriptors.end() || tracked->second.uav != is_uav(range.type))
                        continue;
                    const auto &desc = tracked->second;
                    const auto live = textures.find(desc.resource);
                    if (live == textures.end() || live->second.id != desc.id)
                        continue;
                    append(list, line(desc.uav ? "uav" : "srv", first + i, range.dx_register_space,
                                      range.dx_register_index + j, emit_texture(live->second),
                                      static_cast<uint32_t>(desc.format)));
                }
            }
        }
    });
}

void root_descriptor(a::command_list *cmd, a::shader_stage stages, a::pipeline_layout layout,
                     uint32_t parameter, const a::descriptor_table_update &update)
{
    if (update.type != a::descriptor_type::constant_buffer || update.count != 1 ||
        update.descriptors == nullptr)
        return;
    on_list(cmd, [&](List &list) {
        const auto &buffer = *static_cast<const a::buffer_range *>(update.descriptors);
        append(list, line("root_cbv", static_cast<uint32_t>(stages), layout.handle, parameter,
                          buffer.buffer.handle, buffer.offset), true);
    });
}

void bind_pipeline(a::command_list *cmd, a::pipeline_stage stages, a::pipeline pipeline)
{
    if (stages == a::pipeline_stage::all_ray_tracing)
        return;
    on_list(cmd, [&](List &list) { list.pipeline = pipeline.handle; });
    on_mask_list(cmd, [&](MaskList &list) {
        const auto it = pipelines.find(pipeline.handle);
        list.pixel_shader = it != pipelines.end() ? it->second.pixel_shader : 0;
    });
}

void targets(a::command_list *cmd, const char *kind, uint32_t count,
             const a::resource_view *colors, a::resource_view depth)
{
    on_list(cmd, [&](List &list) {
        auto *device = cmd->get_device();
        const auto view_id = [&](a::resource_view view) {
            return view.handle != 0 ? emit_resource(device, device->get_resource_from_view(view)) : 0;
        };
        std::string row = line(kind, view_id(depth), count);
        for (uint32_t i = 0; i < count; ++i)
            row += '\t' + std::to_string(view_id(colors[i]));
        append(list, std::move(row), true);
    });
}

void bind_targets(a::command_list *cmd, uint32_t count, const a::resource_view *colors, a::resource_view depth)
{
    targets(cmd, "targets", count, colors, depth);
    on_mask_list(cmd, [&](MaskList &list) {
        list.target = count == 1 && depth.handle == 0 ? colors[0] : a::resource_view{};
        list.target_resource = list.target.handle ? cmd->get_device()->get_resource_from_view(list.target)
                                                  : a::resource{};
    });
}

void begin_pass(a::command_list *cmd, uint32_t count, const a::render_pass_render_target_desc *colors,
                const a::render_pass_depth_stencil_desc *depth)
{
    a::resource_view views[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    const uint32_t limit = std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (uint32_t i = 0; i < limit; ++i)
        views[i] = colors[i].view;
    targets(cmd, "begin_pass", limit, views, depth != nullptr ? depth->view : a::resource_view{});
    on_mask_list(cmd, [](MaskList &list) {
        list.render_pass_open = true;
        list.stage = MaskStage::waiting;
    });
}

void end_pass(a::command_list *cmd)
{
    on_list(cmd, [](List &list) { append(list, "end_pass"); });
    on_mask_list(cmd, [](MaskList &list) {
        list.render_pass_open = false;
        list.target = {};
        list.target_resource = {};
    });
}

void viewport(a::command_list *cmd, uint32_t first, uint32_t count, const a::viewport *views)
{
    on_list(cmd, [&](List &list) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto &v = views[i];
            append(list, line("viewport", first + i, v.x, v.y, v.width, v.height, v.min_depth, v.max_depth), true);
        }
    });
    on_mask_list(cmd, [&](MaskList &list) {
        list.viewport = first == 0 && count == 1 ? views[0] : a::viewport{};
    });
}

void barrier(a::command_list *cmd, uint32_t count, const a::resource *res,
             const a::resource_usage *before, const a::resource_usage *after)
{
    on_list(cmd, [&](List &list) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto id = emit_resource(cmd->get_device(), res[i]);
            if (id)
                append(list, line("barrier", id, static_cast<uint32_t>(before[i]), static_cast<uint32_t>(after[i])));
        }
    });
}

bool clear_depth(a::command_list *cmd, a::resource_view view, const float *depth, const uint8_t *,
                 uint32_t rect_count, const a::rect *)
{
    if (depth == nullptr || view.handle == 0)
        return false;
    on_list(cmd, [&](List &list) {
        auto *device = cmd->get_device();
        append(list, line("depth_clear", emit_resource(device, device->get_resource_from_view(view)),
                          *depth, rect_count));
    });
    return false;
}

bool copy_resource(a::command_list *cmd, a::resource source, a::resource dest)
{
    on_list(cmd, [&](List &list) {
        const auto from = emit_resource(cmd->get_device(), source);
        const auto to = emit_resource(cmd->get_device(), dest);
        if (from && to)
            append(list, line("copy", from, to));
    });
    return false;
}

bool copy_texture(a::command_list *cmd, a::resource source, uint32_t source_subresource,
                  const a::subresource_box *, a::resource dest, uint32_t dest_subresource,
                  const a::subresource_box *, a::filter_mode)
{
    on_list(cmd, [&](List &list) {
        const auto from = emit_resource(cmd->get_device(), source);
        const auto to = emit_resource(cmd->get_device(), dest);
        if (from && to)
            append(list, line("copy_region", from, to, source_subresource, dest_subresource));
    });
    return false;
}

bool is_mask_target(a::device *device, const MaskList &list, uint32_t width, uint32_t height)
{
    const auto &v = list.viewport;
    if (!list.target_resource.handle || list.render_pass_open || v.x != 0 || v.y != 0 ||
        v.width != float(width) || v.height != float(height) ||
        device->get_resource_from_view(list.target) != list.target_resource)
        return false;

    const auto view = device->get_resource_view_desc(list.target);
    if (view.type != a::resource_view_type::texture_2d || view.format != a::format::r8_unorm ||
        view.texture.first_level != 0 || view.texture.first_layer != 0)
        return false;

    const auto &desc = texture(device, list.target_resource).desc;
    return screen_texture(desc) && desc.texture.width == width && desc.texture.height == height &&
           desc.texture.samples == 1 && desc.texture.levels == 1 && desc.texture.depth_or_layers == 1 &&
           (desc.texture.format == a::format::r8_typeless || desc.texture.format == a::format::r8_unorm);
}

struct ClearMask {
    a::resource_view target{};
    float value = 0;
};

ClearMask observe_draw(a::command_list *cmd, uint32_t instance_count, uint32_t element_count)
{
    on_list(cmd, [](List &list) {
        const auto it = pipelines.find(list.pipeline);
        append(list, line("draw", list.pipeline, it != pipelines.end() ? it->second.pixel_shader : 0,
                          list.graphics_layout), true);
    });

    ClearMask clear;
    on_mask_list(cmd, [&](MaskList &list) {
        const auto previous = std::exchange(list.stage, MaskStage::waiting);
        if (list.modified || instance_count != 1 || element_count == 0)
            return;

        auto *device = cmd->get_device();
        if (list.pixel_shader == half_mask_shader &&
            is_mask_target(device, list, probe_width / 2, probe_height / 2)) {
            list.stage = MaskStage::half;
            list.half_resource = list.target_resource;
        } else if (list.pixel_shader == blur_mask_shader && previous == MaskStage::half &&
                   list.target_resource != list.half_resource && is_mask_target(device, list, probe_width / 2, probe_height / 2)) {
            list.stage = MaskStage::blurred;
        } else if (list.pixel_shader == upsample_mask_shader && previous == MaskStage::blurred &&
                   GetForegroundWindow() == mask_probe.window &&
                   is_mask_target(device, list, probe_width, probe_height)) {
            list.modified = true;
            clear = {list.target, mask_probe.mode == MaskOverride::white ? 1.0f : 0.0f};
        }
    });
    return clear;
}

bool draw(a::command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance)
{
    const auto clear = observe_draw(cmd, instances, vertices);
    if (!clear.target.handle)
        return false;
    // The add-on API records directly on this DX12 list without re-entering the event.
    // Preserve the native draw, then override only its single R8 target.
    cmd->draw(vertices, instances, first, first_instance);
    const float color[]{clear.value, clear.value, clear.value, clear.value};
    cmd->clear_render_target_view(clear.target, color);
    return true;
}

bool draw_indexed(a::command_list *cmd, uint32_t indices, uint32_t instances, uint32_t first,
                  int32_t base, uint32_t first_instance)
{
    const auto clear = observe_draw(cmd, instances, indices);
    if (!clear.target.handle)
        return false;
    cmd->draw_indexed(indices, instances, first, base, first_instance);
    const float color[]{clear.value, clear.value, clear.value, clear.value};
    cmd->clear_render_target_view(clear.target, color);
    return true;
}

bool dispatch(a::command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{
    on_mask_list(cmd, [](MaskList &list) { list.stage = MaskStage::waiting; });
    on_list(cmd, [&](List &list) {
        const auto it = pipelines.find(list.pipeline);
        append(list, line("dispatch", list.pipeline, it != pipelines.end() ? it->second.compute_shader : 0,
                          list.compute_layout, x, y, z), true);
    });
    return false;
}

bool indirect(a::command_list *cmd, a::indirect_command type, a::resource, uint64_t, uint32_t count, uint32_t)
{
    on_mask_list(cmd, [](MaskList &list) { list.stage = MaskStage::waiting; });
    on_list(cmd, [&](List &list) {
        const auto it = pipelines.find(list.pipeline);
        append(list, line("indirect", list.pipeline,
                          it != pipelines.end() ? it->second.pixel_shader : 0,
                          it != pipelines.end() ? it->second.compute_shader : 0,
                          static_cast<uint32_t>(type), count), true);
    });
    return false;
}

void forget_list(a::command_list *cmd)
{
    if (!capturing.load(std::memory_order_relaxed) && !probing.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    const auto it = capture.lists.find(cmd);
    if (it != capture.lists.end()) {
        capture.live_rows -= it->second.rows.size();
        capture.lists.erase(it);
    }
    mask_probe.lists.erase(cmd);
}

void init_list(a::command_list *cmd)
{
    if (!probing.load(std::memory_order_relaxed) || !dx12(cmd->get_device()))
        return;
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    const std::lock_guard lock(mutex);
    if (mask_probe.swapchain && cmd->get_device() == mask_probe.swapchain->get_device())
        mask_probe.lists.try_emplace(cmd);
}

void reset_list(a::command_list *cmd)
{
    forget_list(cmd);
    init_list(cmd);
}

void bundle(a::command_list *parent, a::command_list *child)
{
    on_mask_list(parent, [](MaskList &list) {
        list.stage = MaskStage::waiting;
        list.pixel_shader = 0;
    });
    on_list(parent, [&](List &list) {
        const auto it = capture.lists.find(child);
        append(list, line("begin_bundle", reinterpret_cast<uintptr_t>(child), it != capture.lists.end()));
        if (it != capture.lists.end() && child != parent) {
            const List &recorded = it->second;
            list.dropped += recorded.dropped;
            for (const auto &row : recorded.rows) {
                if (list.rows.size() < max_list_rows && capture.live_rows < max_live_rows) {
                    list.rows.push_back(row);
                    ++capture.live_rows;
                } else {
                    ++list.dropped;
                }
            }
        }
        append(list, "end_bundle");
        // A bundle may set the pipeline and root state inherited by the parent.
        list.pipeline = list.graphics_layout = list.compute_layout = 0;
    });
}

void submit(a::command_queue *queue, a::command_list *cmd)
{
    const char *message = nullptr;
    on_mask_list(cmd, [&](MaskList &list) {
        if (list.modified && !mask_probe.reported) {
            mask_probe.reported = true;
            message = mask_probe.mode == MaskOverride::white
                ? "ZZZ AO mask probe submitted a white override."
                : "ZZZ AO mask probe submitted a black override.";
        }
    });
    if (message)
        log::message(log::level::info, message);

    if (!capturing.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    if (capture.device == nullptr || queue->get_device() != capture.device)
        return;
    const auto it = capture.lists.find(cmd);
    append_trace(line("submit", reinterpret_cast<uintptr_t>(queue), static_cast<uint32_t>(queue->get_type()),
                      reinterpret_cast<uintptr_t>(cmd), it != capture.lists.end()));
    if (it == capture.lists.end()) {
        ++capture.unobserved_lists;
        return;
    }
    for (const auto &row : it->second.rows)
        append_trace(row.text + '\t' + std::to_string(row.repeat));
    if (it->second.dropped)
        append_trace(line("list_truncated", it->second.dropped));
}

void save(Capture &finished)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("zzz-ao-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(finished.id) + ".tsv");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto &row : finished.rows)
        out << row << '\n';
    out << line("summary", finished.presents, finished.unobserved_lists, finished.dropped) << '\n';
    out.close();
    if (out)
        log::message(log::level::info, ("ZZZ AO capture saved: " + path.string()).c_str());
    else
        log::message(log::level::error, "ZZZ AO could not write the capture to the Windows temporary folder.");
}

void present(a::command_queue *queue, a::swapchain *swapchain, const a::rect *, const a::rect *,
             uint32_t, const a::rect *)
{
    auto *device = queue->get_device();
    if (!dx12(device))
        return;
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const auto window = static_cast<HWND>(swapchain->get_hwnd());
    const bool focused = window && window == GetForegroundWindow();
    const auto mode = focused && (GetAsyncKeyState(VK_F9) & 0x8000) != 0
        ? ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ? MaskOverride::black : MaskOverride::white)
        : MaskOverride::off;

    Capture finished;
    bool probe_missed = false;
    {
        const std::lock_guard lock(mutex);
        if ((focused || mask_probe.swapchain == swapchain) &&
            (mask_probe.mode != mode || (mode != MaskOverride::off && mask_probe.swapchain != swapchain))) {
            probe_missed = mask_probe.mode != MaskOverride::off && !mask_probe.reported;
            mask_probe = {};
            if (mode != MaskOverride::off) {
                mask_probe.swapchain = swapchain;
                mask_probe.window = window;
                mask_probe.mode = mode;
            }
            probing.store(mode != MaskOverride::off, std::memory_order_relaxed);
        }
        if (capture.swapchain == swapchain) {
            ++capture.presents;
            append_trace(line("present", capture.presents, emit_resource(device, swapchain->get_current_back_buffer())));
            if (capture.presents == interval_count) {
                append_trace(line("descriptor_limit_reached", descriptor_limit_reached));
                capturing.store(false, std::memory_order_relaxed);
                finished = std::exchange(capture, Capture{});
            }
        }
        if (focused) {
            if (down && !f10_down && capture.device == nullptr && finished.id == 0) {
                capture.swapchain = swapchain;
                capture.device = device;
                capture.id = ++next_capture_id;
                append_trace(line("zzz_ao_capture", 1, interval_count));
                append_trace(line("begin", emit_resource(device, swapchain->get_current_back_buffer())));
                capturing.store(true, std::memory_order_relaxed);
            }
            f10_down = down;
        }
    }
    if (probe_missed)
        log::message(log::level::info, "ZZZ AO mask probe found no matching submitted draw.");
    if (finished.id != 0)
        save(finished);
}

void destroy_swapchain(a::swapchain *swapchain, bool)
{
    Capture finished;
    {
        const std::lock_guard lock(mutex);
        if (mask_probe.swapchain == swapchain) {
            probing.store(false, std::memory_order_relaxed);
            mask_probe = {};
        }
        if (capture.swapchain != swapchain)
            return;
        append_trace("swapchain_destroyed");
        capturing.store(false, std::memory_order_relaxed);
        finished = std::exchange(capture, Capture{});
    }
    save(finished);
}

// Never let a diagnostics allocation or file exception leave an add-on callback.
template <auto Function, typename R, typename... Args>
constexpr auto guarded(R (*)(Args...))
{
    return +[](Args... args) -> R {
        try {
            return Function(args...);
        } catch (...) {
            if constexpr (!std::is_void_v<R>)
                return R{};
        }
    };
}

template <addon_event Event, auto Function>
void observe()
{
    register_event<Event>(guarded<Function>(Function));
}
} // namespace

extern "C" __declspec(dllexport) const char *NAME = "ZZZ DX12 AO Capture";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "F10 captures a DX12 trace. Hold F9 to test the ZZZ 3.2 lighting mask.";

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        if (!reshade::register_addon(module))
            return FALSE;

        observe<addon_event::init_resource, init_resource>();
        observe<addon_event::destroy_resource, destroy_resource>();
        observe<addon_event::init_pipeline, init_pipeline>();
        observe<addon_event::destroy_pipeline, destroy_pipeline>();
        observe<addon_event::init_pipeline_layout, init_layout>();
        observe<addon_event::destroy_pipeline_layout, destroy_layout>();
        observe<addon_event::update_descriptor_tables, update_descriptors>();
        observe<addon_event::copy_descriptor_tables, copy_descriptors>();
        observe<addon_event::bind_descriptor_tables, bind_tables>();
        observe<addon_event::push_descriptors, root_descriptor>();
        observe<addon_event::bind_pipeline, bind_pipeline>();
        observe<addon_event::bind_render_targets_and_depth_stencil, bind_targets>();
        observe<addon_event::begin_render_pass, begin_pass>();
        observe<addon_event::end_render_pass, end_pass>();
        observe<addon_event::bind_viewports, viewport>();
        observe<addon_event::barrier, barrier>();
        observe<addon_event::clear_depth_stencil_view, clear_depth>();
        observe<addon_event::copy_resource, copy_resource>();
        observe<addon_event::copy_texture_region, copy_texture>();
        observe<addon_event::draw, draw>();
        observe<addon_event::draw_indexed, draw_indexed>();
        observe<addon_event::dispatch, dispatch>();
        observe<addon_event::draw_or_dispatch_indirect, indirect>();
        observe<addon_event::init_command_list, init_list>();
        observe<addon_event::reset_command_list, reset_list>();
        observe<addon_event::destroy_command_list, forget_list>();
        observe<addon_event::execute_secondary_command_list, bundle>();
        observe<addon_event::execute_command_list, submit>();
        observe<addon_event::present, present>();
        observe<addon_event::destroy_swapchain, destroy_swapchain>();
        reshade::log::message(reshade::log::level::info, "ZZZ AO DX12 diagnostics loaded. Press F10 for a trace or hold F9 for the ZZZ 3.2 mask test.");
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
