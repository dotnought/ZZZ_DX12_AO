#include <d3d12.h>
#include <imgui.h>
#include <reshade.hpp>

#include "character_ao.hpp"
#include "character_ao_ps.hpp"
#include "shading_view_ps.hpp"
#ifdef ZZZ_AO_ENABLE_TRACE
#include "depth_capture.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
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
using zzz_ao::ShadingPass;

#ifdef ZZZ_AO_ENABLE_TRACE
constexpr unsigned interval_count = 4;
constexpr unsigned max_depth_intervals = interval_count * 2;
constexpr size_t max_list_rows = 24000;
constexpr size_t max_trace_rows = 120000;
constexpr size_t max_live_rows = 120000;
#endif

constexpr size_t max_descriptor_slots = 524288;
#ifdef ZZZ_AO_ENABLE_TRACE
constexpr uint32_t max_range_slots = 64;
#endif
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
    uint64_t layout = 0;
    zzz_ao::CharacterAo character_ao;
    a::format color_format = a::format::unknown;
};

// These two passes identify the depth source and the pre-DLSS scene color in ZZZ 3.2.
constexpr uint64_t capsule_ao_shader = 2471263705428555365ull;
constexpr uint64_t prescale_scene_shader = 7607800794214941815ull;

#ifdef ZZZ_AO_ENABLE_TRACE
struct NativeShader {
    uint64_t fingerprint;
    const char *name;
};
constexpr auto native_shaders = std::to_array<NativeShader>({
    {6709996353822950300ull, "ps_before_half_mask"},
    {10793278501038871532ull, "ps_half_mask"},
    {18023612121467718113ull, "ps_blur_mask"},
    {9317972913748041275ull, "ps_upsample_mask"},
    {16383356142044416029ull, "ps_lighting_candidate"},
    {capsule_ao_shader, "cs_half_resolution_mask"},
    {15181696564642202686ull, "ps_after_world_lighting_1"},
    {11889072915614133341ull, "ps_after_world_lighting_2"},
    {12916642976149190619ull, "ps_late_scene_geometry"},
    {16133994147759704127ull, "ps_late_scene_overlay"},
    {prescale_scene_shader, "ps_prescale_scene_color"},
});
constexpr size_t max_native_shader_bytes = 256 * 1024;
using NativeShaderSet = std::bitset<native_shaders.size()>;

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
    NativeShaderSet native_shaders_seen;
    a::resource depth_target{};
    uint32_t color_target_count = 0;
    bool explicit_render_pass = false;
    a::resource depth_copy_candidate{};
    a::resource live_depth{};
    a::resource before_ao_depth{};
    a::resource_usage live_depth_state = a::resource_usage::undefined;
    bool prescale_scene_seen = false;
};

#endif

constexpr uint32_t probe_width = 1720;
constexpr uint32_t probe_height = 720;

struct CharacterInputs {
    a::resource depth_target{};
    a::resource copy_candidate{};
    a::resource early_depth{};
    a::resource_usage depth_state = a::resource_usage::undefined;
    uint64_t compute_layout = 0;
    a::descriptor_table capsule_table{};
    a::descriptor_table depth_srv{};
    uint64_t depth_heap = 0;
    std::array<a::descriptor_table, 3> graphics_tables{};
};

struct AoList {
    uint64_t pixel_shader = 0;
    uint64_t graphics_layout = 0;
    a::pipeline pipeline{};
    a::resource_view target{};
    a::resource target_resource{};
    a::viewport viewport{};
    bool render_pass_open = false;
    bool modified = false;
    CharacterInputs character;
};

struct AoRuntime {
    a::swapchain *swapchain = nullptr;
    HWND window = nullptr;
    bool reported = false;
    ShadingPass pass = ShadingPass::composite;
    std::unordered_map<a::command_list *, AoList> lists;
};

struct Controls {
    a::effect_runtime *runtime = nullptr;
    bool detail_ao = false;
    bool shading_view = false;
#ifdef ZZZ_AO_ENABLE_TRACE
    bool capture_requested = false;
    bool overlay_open = false;
#endif
};

struct HeapSlot {
    uint64_t heap = 0;
    uint32_t index = 0;
    uint32_t capacity = 0;

    uint64_t key(uint32_t offset = 0) const { return (heap << 32) | (index + offset); }
};

#ifdef ZZZ_AO_ENABLE_TRACE
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
    NativeShaderSet native_shaders_seen;
    std::shared_ptr<zzz_ao::DepthCapture> depth;
    bool depth_copy_point_seen = false;
};
#endif

#ifdef ZZZ_AO_ENABLE_TRACE
std::atomic_bool capturing{false};
std::atomic_bool depth_pending{false};
Capture capture;
std::shared_ptr<zzz_ao::DepthCapture> pending_depth;
std::array<std::vector<uint8_t>, native_shaders.size()> native_shader_code;
uint64_t next_capture_id = 0;
#endif
std::atomic_bool ao_active{false};
AoRuntime ao_runtime;
Controls controls;
std::mutex mutex;
std::unordered_map<uint64_t, Texture> textures;
std::unordered_map<uint64_t, Pipeline> pipelines;
std::unordered_map<uint64_t, std::vector<std::vector<a::descriptor_range>>> layouts;
std::unordered_map<uint64_t, Descriptor> descriptors;
uint64_t next_texture_id = 0;
uint32_t next_heap_id = 0;
#ifdef ZZZ_AO_ENABLE_TRACE
bool descriptor_limit_reached = false;
#endif

#ifdef ZZZ_AO_ENABLE_TRACE
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
#endif

bool dx12(a::device *device)
{
    return device->get_api() == a::device_api::d3d12;
}

#ifdef ZZZ_AO_ENABLE_TRACE
template <typename Function>
void on_list(a::command_list *cmd, const Function &callback)
{
    if (!capturing.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    if (capture.device != nullptr && cmd->get_device() == capture.device)
        callback(capture.lists[cmd]);
}
#endif

template <typename Function>
void on_ao_list(a::command_list *cmd, const Function &callback)
{
    if (!ao_active.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    if (const auto state = ao_runtime.lists.find(cmd); state != ao_runtime.lists.end())
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

#ifdef ZZZ_AO_ENABLE_TRACE
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
#endif

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

#ifdef ZZZ_AO_ENABLE_TRACE
size_t native_shader_index(uint64_t fingerprint)
{
    return static_cast<size_t>(std::find_if(native_shaders.begin(), native_shaders.end(),
        [&](const NativeShader &shader) { return shader.fingerprint == fingerprint; }) - native_shaders.begin());
}

void cache_native_shader(uint64_t hash, const a::shader_desc *shader)
{
    const size_t index = native_shader_index(hash);
    if (index == native_shaders.size() || !shader || !shader->code ||
        shader->code_size == 0 || shader->code_size > max_native_shader_bytes ||
        !native_shader_code[index].empty())
        return;
    const auto *bytes = static_cast<const uint8_t *>(shader->code);
    native_shader_code[index].assign(bytes, bytes + shader->code_size);
}
#endif

void init_pipeline(a::device *device, a::pipeline_layout layout, uint32_t count,
                   const a::pipeline_subobject *objects, a::pipeline pipeline)
{
    if (!dx12(device))
        return;
    Pipeline info;
    info.layout = layout.handle;
    const a::shader_desc *pixel_shader = nullptr;
    const a::shader_desc *compute_shader = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &item = objects[i];
        if (item.count != 1 || item.data == nullptr)
            continue;
        if (item.type == a::pipeline_subobject_type::pixel_shader) {
            pixel_shader = static_cast<const a::shader_desc *>(item.data);
            info.pixel_shader = fingerprint(*pixel_shader);
        }
        if (item.type == a::pipeline_subobject_type::compute_shader) {
            compute_shader = static_cast<const a::shader_desc *>(item.data);
            info.compute_shader = fingerprint(*compute_shader);
        }
        if (item.type == a::pipeline_subobject_type::render_target_formats)
            info.color_format = *static_cast<const a::format *>(item.data);
    }

    if (info.pixel_shader == prescale_scene_shader && layout.handle &&
               (info.color_format == a::format::r8g8b8a8_unorm || info.color_format == a::format::r8g8b8a8_unorm_srgb)) {
        auto ao = zzz_ao::CharacterAo::Create(reinterpret_cast<ID3D12Device *>(device->get_native()));
        if (ao.Root()) {
            std::vector<a::pipeline_subobject> replacement;
            replacement.reserve(count + 1);
            for (uint32_t i = 0; i < count; ++i) {
                if (objects[i].type != a::pipeline_subobject_type::blend_state)
                    replacement.push_back(objects[i]);
            }
            a::shader_desc shading = *pixel_shader;
            const auto shader = std::ranges::find(replacement, a::pipeline_subobject_type::pixel_shader,
                                                   &a::pipeline_subobject::type);
            shader->data = &shading;
            a::blend_desc blend;
            blend.render_target_write_mask[0] = 0x7;
            replacement.push_back({a::pipeline_subobject_type::blend_state, 1, &blend});

            const a::pipeline_layout character_root{reinterpret_cast<uint64_t>(ao.Root())};
            const auto create_pass = [&](ShadingPass pass, const auto &bytecode) {
                shading.code = bytecode;
                shading.code_size = sizeof(bytecode);
                blend.blend_enable[0] = pass == ShadingPass::composite;
                blend.source_color_blend_factor[0] = blend.blend_enable[0] ? a::blend_factor::dest_color : a::blend_factor::one;
                blend.dest_color_blend_factor[0] = blend.blend_enable[0] ? a::blend_factor::source_alpha : a::blend_factor::zero;
                a::pipeline variant{};
                if (device->create_pipeline(character_root, static_cast<uint32_t>(replacement.size()),
                                            replacement.data(), &variant))
                    ao.Attach(pass, reinterpret_cast<ID3D12PipelineState *>(variant.handle));
            };
            create_pass(ShadingPass::composite, character_ao_ps_bytecode);
            create_pass(ShadingPass::preview, shading_view_ps_bytecode);
            info.character_ao = std::move(ao);
        }
        if (!info.character_ao.HasPipeline(ShadingPass::composite))
            log::message(log::level::warning, "ZZZ AO could not create the Detail AO pipeline.");
        if (!info.character_ao.HasPipeline(ShadingPass::preview))
            log::message(log::level::warning, "ZZZ AO could not create the Shading View pipeline.");
    }

    const std::lock_guard lock(mutex);
#ifdef ZZZ_AO_ENABLE_TRACE
    cache_native_shader(info.pixel_shader, pixel_shader);
    cache_native_shader(info.compute_shader, compute_shader);
#endif
    pipelines.insert_or_assign(pipeline.handle, std::move(info));
}

void destroy_pipeline(a::device *device, a::pipeline pipeline)
{
    if (!dx12(device))
        return;
    Pipeline retired;
    {
        const std::lock_guard lock(mutex);
        if (const auto it = pipelines.find(pipeline.handle); it != pipelines.end()) {
            retired = std::move(it->second);
            pipelines.erase(it);
        }
    }
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
#ifdef ZZZ_AO_ENABLE_TRACE
        descriptor_limit_reached = true;
#endif
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
    const bool graphics = (stages & a::shader_stage::all_graphics) != static_cast<a::shader_stage>(0);
    const bool compute = (stages & a::shader_stage::compute) != static_cast<a::shader_stage>(0);
    on_ao_list(cmd, [&](AoList &list) {
        auto &inputs = list.character;
        if (graphics) {
            if (list.graphics_layout != layout.handle)
                inputs.graphics_tables = {};
            for (uint32_t i = 0; i < count && size_t(first) + i < inputs.graphics_tables.size(); ++i)
                inputs.graphics_tables[first + i] = tables[i];
            list.graphics_layout = layout.handle;
        }
        if (compute) {
            if (inputs.compute_layout != layout.handle)
                inputs.capsule_table = {};
            inputs.compute_layout = layout.handle;
            if (count != 0 && first == 0)
                inputs.capsule_table = tables[0];
        }
    });
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [&](List &list) {
        if (graphics)
            list.graphics_layout = layout.handle;
        if (compute)
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
#endif
}

#ifdef ZZZ_AO_ENABLE_TRACE
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

#endif

void bind_pipeline(a::command_list *cmd, a::pipeline_stage stages, a::pipeline pipeline)
{
    if (stages == a::pipeline_stage::all_ray_tracing)
        return;
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [&](List &list) { list.pipeline = pipeline.handle; });
#endif
    on_ao_list(cmd, [&](AoList &list) {
        const auto it = pipelines.find(pipeline.handle);
        list.pixel_shader = it != pipelines.end() ? it->second.pixel_shader : 0;
        list.pipeline = pipeline;
    });
}

#ifdef ZZZ_AO_ENABLE_TRACE
void targets(a::command_list *cmd, const char *kind, uint32_t count,
             const a::resource_view *colors, a::resource_view depth, bool explicit_pass = false)
{
    on_list(cmd, [&](List &list) {
        auto *device = cmd->get_device();
        list.depth_target = depth.handle ? device->get_resource_from_view(depth) : a::resource{};
        list.color_target_count = count;
        list.explicit_render_pass = explicit_pass;
        const auto view_id = [&](a::resource_view view) {
            return view.handle != 0 ? emit_resource(device, device->get_resource_from_view(view)) : 0;
        };
        std::string row = line(kind, emit_resource(device, list.depth_target), count);
        for (uint32_t i = 0; i < count; ++i)
            row += '\t' + std::to_string(view_id(colors[i]));
        append(list, std::move(row), true);
    });
}
#endif

void bind_targets(a::command_list *cmd, uint32_t count, const a::resource_view *colors, a::resource_view depth)
{
#ifdef ZZZ_AO_ENABLE_TRACE
    targets(cmd, "targets", count, colors, depth);
#endif
    on_ao_list(cmd, [&](AoList &list) {
        auto *device = cmd->get_device();
        list.target = count == 1 && depth.handle == 0 ? colors[0] : a::resource_view{};
        list.target_resource = list.target.handle ? device->get_resource_from_view(list.target) : a::resource{};
        list.character.depth_target = count == 4 && depth.handle ? device->get_resource_from_view(depth) : a::resource{};
    });
}

void begin_pass(a::command_list *cmd, uint32_t count, const a::render_pass_render_target_desc *colors,
                const a::render_pass_depth_stencil_desc *depth)
{
#ifdef ZZZ_AO_ENABLE_TRACE
    a::resource_view views[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    const uint32_t limit = std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (uint32_t i = 0; i < limit; ++i)
        views[i] = colors[i].view;
    targets(cmd, "begin_pass", limit, views, depth != nullptr ? depth->view : a::resource_view{}, true);
#else
    (void)count;
    (void)colors;
    (void)depth;
#endif
    on_ao_list(cmd, [](AoList &list) {
        list.render_pass_open = true;
        list.character.depth_target = {};
    });
}

void end_pass(a::command_list *cmd)
{
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [](List &list) {
        list.explicit_render_pass = false;
        list.depth_target = {};
        list.color_target_count = 0;
        append(list, "end_pass");
    });
#endif
    on_ao_list(cmd, [](AoList &list) {
        list.render_pass_open = false;
        list.target = {};
        list.target_resource = {};
    });
}

void viewport(a::command_list *cmd, uint32_t first, uint32_t count, const a::viewport *views)
{
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [&](List &list) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto &v = views[i];
            append(list, line("viewport", first + i, v.x, v.y, v.width, v.height, v.min_depth, v.max_depth), true);
        }
    });
#endif
    on_ao_list(cmd, [&](AoList &list) {
        list.viewport = first == 0 && count == 1 ? views[0] : a::viewport{};
    });
}

void barrier(a::command_list *cmd, uint32_t count, const a::resource *res,
             const a::resource_usage *before, const a::resource_usage *after)
{
    on_ao_list(cmd, [&](AoList &list) {
        auto &inputs = list.character;
        for (uint32_t i = 0; i < count; ++i) {
            if (res[i] == inputs.copy_candidate)
                inputs.copy_candidate = {};
            if (inputs.depth_target.handle && res[i] == inputs.depth_target &&
                before[i] == a::resource_usage::depth_stencil_write && after[i] == a::resource_usage::copy_source)
                inputs.copy_candidate = res[i];
            if (inputs.early_depth.handle && res[i] == inputs.early_depth) {
                inputs.depth_state = after[i];
                if (after[i] == a::resource_usage::copy_dest || after[i] == a::resource_usage::undefined)
                    inputs.depth_srv = {};
            }
        }
    });
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [&](List &list) {
        for (uint32_t i = 0; i < count; ++i) {
            if (res[i] == list.depth_copy_candidate)
                list.depth_copy_candidate = {};
            if (res[i].handle && res[i] == list.depth_target && list.color_target_count == 4 &&
                before[i] == a::resource_usage::depth_stencil_write &&
                after[i] == a::resource_usage::copy_source)
                list.depth_copy_candidate = res[i];
            if (res[i].handle && res[i] == list.live_depth)
                list.live_depth_state = after[i];
            const auto id = emit_resource(cmd->get_device(), res[i]);
            if (id)
                append(list, line("barrier", id, static_cast<uint32_t>(before[i]), static_cast<uint32_t>(after[i])));
        }
    });
#endif
}

#ifdef ZZZ_AO_ENABLE_TRACE
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
#endif

bool full_scene_depth(a::device *device, a::resource resource)
{
    if (!resource.handle)
        return false;
    const auto &desc = texture(device, resource).desc;
    return screen_texture(desc) && desc.texture.format == a::format::r32_g8_typeless &&
           desc.texture.width == probe_width && desc.texture.height == probe_height &&
           desc.texture.samples == 1 && desc.texture.levels == 1 && desc.texture.depth_or_layers == 1;
}

#ifdef ZZZ_AO_ENABLE_TRACE
void record_character_depth(a::command_list *cmd, List &list, a::resource source, a::resource dest)
{
    if (capture.depth || pending_depth || list.explicit_render_pass)
        return;

    if (source.handle && source == list.depth_copy_candidate) {
        list.depth_copy_candidate = {};
        if (full_scene_depth(cmd->get_device(), source) && full_scene_depth(cmd->get_device(), dest)) {
            list.live_depth = source;
            list.before_ao_depth = dest;
            list.live_depth_state = a::resource_usage::copy_source;
            list.prescale_scene_seen = false;
        }
        return;
    }

    // The game is about to read its earlier depth copy again for upscaling.
    // Its live depth is now unbound, with the DEPTH_WRITE state observed.
    if (!list.prescale_scene_seen || list.depth_target.handle || !source.handle ||
        source != list.before_ao_depth || list.live_depth_state != a::resource_usage::depth_stencil_write)
        return;
    capture.depth_copy_point_seen = true;
    auto job = zzz_ao::DepthCapture::Record(
        reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native()),
        reinterpret_cast<ID3D12Resource *>(source.handle),
        reinterpret_cast<ID3D12Resource *>(list.live_depth.handle));
    if (job) {
        capture.depth = job;
        pending_depth = std::move(job);
        depth_pending.store(true, std::memory_order_relaxed);
    }
}
#endif

bool copy_resource(a::command_list *cmd, a::resource source, a::resource dest)
{
    on_ao_list(cmd, [&](AoList &list) {
        auto &inputs = list.character;
        if (list.render_pass_open || !source.handle || source != inputs.copy_candidate)
            return;
        inputs.copy_candidate = {};
        if (full_scene_depth(cmd->get_device(), source) && full_scene_depth(cmd->get_device(), dest)) {
            inputs.early_depth = dest;
            inputs.depth_state = a::resource_usage::copy_dest;
            inputs.depth_srv = {};
        }
    });
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [&](List &list) {
        const auto from = emit_resource(cmd->get_device(), source);
        const auto to = emit_resource(cmd->get_device(), dest);
        if (from && to)
            append(list, line("copy", from, to));
        record_character_depth(cmd, list, source, dest);
    });
#endif
    return false;
}

#ifdef ZZZ_AO_ENABLE_TRACE
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
#endif

struct AoDraw {
    a::pipeline original{};
    zzz_ao::CharacterAo character;
    ShadingPass pass = ShadingPass::composite;
    a::pipeline_layout layout{};
    D3D12_GPU_DESCRIPTOR_HANDLE depth{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> tables{};
};

void keep_capsule_depth(a::device *device, const Pipeline &pipeline, CharacterInputs &inputs)
{
    if (!inputs.early_depth.handle || !inputs.capsule_table.handle || inputs.compute_layout != pipeline.layout)
        return;
    const auto layout = layouts.find(pipeline.layout);
    if (layout == layouts.end() || layout->second.size() != 2 || layout->second[0].size() != 2)
        return;
    const auto &range = layout->second[0][0];
    if (!is_srv(range.type) || range.binding != 0 || range.dx_register_index != 0 ||
        range.dx_register_space != 0 || range.count != 4)
        return;

    // The captured CapsuleAO shader reconstructs position from t1.x. Its
    // four-SRV table is placed alongside the preserved native depth barriers.
    const auto slot = heap_slot(device, inputs.capsule_table, 1);
    if (!slot.capacity ||
        (inputs.depth_state & a::resource_usage::shader_resource_non_pixel) == a::resource_usage::undefined)
        return;
    if (const auto tracked = descriptors.find(slot.key()); tracked != descriptors.end()) {
        const auto &descriptor = tracked->second;
        if (descriptor.resource != inputs.early_depth.handle || descriptor.format != a::format::r32_float_x8_uint)
            return;
    }
    UINT increment = reinterpret_cast<ID3D12Device *>(device->get_native())
                         ->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    inputs.depth_srv = {inputs.capsule_table.handle + increment};
    inputs.depth_heap = slot.heap;
}

bool prescale_layout_can_restore(uint64_t handle)
{
    const auto layout = layouts.find(handle);
    if (layout == layouts.end())
        return false;
    const auto &params = layout->second;
    return params.size() == 3 && params[0].size() == 1 && params[1].size() == 2 && params[2].size() == 1 &&
           params[0][0].type == a::descriptor_type::constant_buffer && params[0][0].count == 2 &&
           params[0][0].binding == 0 && params[0][0].dx_register_index == 0 &&
           params[0][0].dx_register_space == 0 && params[0][0].visibility == a::shader_stage::vertex &&
           is_srv(params[1][0].type) && params[1][0].count == 6 &&
           params[1][1].type == a::descriptor_type::constant_buffer && params[1][1].count == 2 &&
           params[2][0].type == a::descriptor_type::sampler && params[2][0].count == 4;
}

void prepare_character_draw(a::device *device, const AoList &list, AoDraw &action)
{
    const auto &inputs = list.character;
    const auto &v = list.viewport;
    if (!list.target_resource.handle || list.render_pass_open || !inputs.depth_srv.handle ||
        (inputs.depth_state & a::resource_usage::shader_resource_pixel) == a::resource_usage::undefined ||
        v.x != 0 || v.y != 0 || v.width != probe_width || v.height != probe_height ||
        std::ranges::any_of(inputs.graphics_tables, [](a::descriptor_table t) { return t.handle == 0; }))
        return;

    const auto pipeline = pipelines.find(list.pipeline.handle);
    const auto pass = ao_runtime.pass;
    if (pipeline == pipelines.end() || !pipeline->second.character_ao.HasPipeline(pass) ||
        pipeline->second.layout != list.graphics_layout ||
        !prescale_layout_can_restore(list.graphics_layout))
        return;
    const auto &desc = texture(device, list.target_resource).desc;
    const auto view = device->get_resource_view_desc(list.target);
    if (!screen_texture(desc) || desc.texture.width != probe_width || desc.texture.height != probe_height ||
        desc.texture.samples != 1 || desc.texture.levels != 1 || desc.texture.depth_or_layers != 1 ||
        device->get_resource_from_view(list.target) != list.target_resource ||
        view.type != a::resource_view_type::texture_2d || view.format != pipeline->second.color_format ||
        view.texture.first_level != 0 || view.texture.first_layer != 0)
        return;

    // The pre-upscale pass and CapsuleAO must use the same bound resource heap.
    if (heap_slot(device, inputs.graphics_tables[1], 0).heap != inputs.depth_heap)
        return;
    action.original = list.pipeline;
    action.character = pipeline->second.character_ao;
    action.pass = pass;
    action.layout = {list.graphics_layout};
    action.depth = {inputs.depth_srv.handle};
    for (size_t i = 0; i < action.tables.size(); ++i)
        action.tables[i] = {inputs.graphics_tables[i].handle};
}

#ifdef ZZZ_AO_ENABLE_TRACE
void mark_native_shader(List &list, uint64_t shader, uint64_t layout)
{
    const size_t index = native_shader_index(shader);
    if (index == native_shaders.size() || list.native_shaders_seen.test(index))
        return;

    list.native_shaders_seen.set(index);
    const auto signature = layouts.find(layout);
    if (signature == layouts.end())
        return;

    for (size_t parameter = 0; parameter < signature->second.size(); ++parameter) {
        for (const auto &range : signature->second[parameter]) {
            append(list, line("native_ao_binding", shader, layout, parameter,
                              static_cast<uint32_t>(range.type), range.binding,
                              range.dx_register_index, range.dx_register_space, range.count,
                              static_cast<uint32_t>(range.visibility)));
        }
    }
}
#endif

AoDraw observe_draw(a::command_list *cmd, uint32_t instance_count, uint32_t element_count)
{
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [](List &list) {
        const auto it = pipelines.find(list.pipeline);
        const uint64_t shader = it != pipelines.end() ? it->second.pixel_shader : 0;
        const uint64_t layout = it != pipelines.end() ? it->second.layout : 0;
        if (shader == prescale_scene_shader && list.before_ao_depth.handle)
            list.prescale_scene_seen = true;
        mark_native_shader(list, shader, list.graphics_layout ? list.graphics_layout : layout);
        append(list, line("draw", list.pipeline, shader, list.graphics_layout), true);
    });

#endif
    AoDraw action;
    on_ao_list(cmd, [&](AoList &list) {
        if (list.modified || instance_count != 1 || element_count == 0 ||
            list.pixel_shader != prescale_scene_shader || GetForegroundWindow() != ao_runtime.window)
            return;
        prepare_character_draw(cmd->get_device(), list, action);
        list.modified = action.original.handle != 0;
    });
    return action;
}

template <typename Draw>
bool apply_ao(a::command_list *cmd, const AoDraw &action, const Draw &execute_draw)
{
    if (!action.original.handle)
        return false;

    // Reissue the matched native draw, then draw shading and restore its state.
    execute_draw();
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    action.character.Bind(action.pass, list, action.tables[0], action.depth);
    execute_draw();
    zzz_ao::CharacterAo::Restore(list, reinterpret_cast<ID3D12PipelineState *>(action.original.handle),
                                reinterpret_cast<ID3D12RootSignature *>(action.layout.handle), action.tables);
    return true;
}

bool draw(a::command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance)
{
    return apply_ao(cmd, observe_draw(cmd, instances, vertices),
                      [&] { cmd->draw(vertices, instances, first, first_instance); });
}

bool draw_indexed(a::command_list *cmd, uint32_t indices, uint32_t instances, uint32_t first,
                  int32_t base, uint32_t first_instance)
{
    return apply_ao(cmd, observe_draw(cmd, instances, indices),
                      [&] { cmd->draw_indexed(indices, instances, first, base, first_instance); });
}

bool dispatch(a::command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{
    on_ao_list(cmd, [&](AoList &list) {
        if (x != (probe_width + 31) / 32 || y != (probe_height + 31) / 32 || z != 1)
            return;
        const auto pipeline = pipelines.find(list.pipeline.handle);
        if (pipeline != pipelines.end() && pipeline->second.compute_shader == capsule_ao_shader)
            keep_capsule_depth(cmd->get_device(), pipeline->second, list.character);
    });
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(cmd, [&](List &list) {
        const auto it = pipelines.find(list.pipeline);
        const uint64_t shader = it != pipelines.end() ? it->second.compute_shader : 0;
        const uint64_t layout = it != pipelines.end() ? it->second.layout : 0;
        mark_native_shader(list, shader, list.compute_layout ? list.compute_layout : layout);
        append(list, line("dispatch", list.pipeline, shader, list.compute_layout, x, y, z), true);
    });
#endif
    return false;
}

#ifdef ZZZ_AO_ENABLE_TRACE
bool indirect(a::command_list *cmd, a::indirect_command type, a::resource, uint64_t, uint32_t count, uint32_t)
{
    on_list(cmd, [&](List &list) {
        const auto it = pipelines.find(list.pipeline);
        append(list, line("indirect", list.pipeline,
                          it != pipelines.end() ? it->second.pixel_shader : 0,
                          it != pipelines.end() ? it->second.compute_shader : 0,
                          static_cast<uint32_t>(type), count), true);
    });
    return false;
}
#endif

void forget_list(a::command_list *cmd)
{
    if (!ao_active.load(std::memory_order_relaxed)
#ifdef ZZZ_AO_ENABLE_TRACE
        && !capturing.load(std::memory_order_relaxed) && !depth_pending.load(std::memory_order_relaxed)
#endif
    )
        return;
    const std::lock_guard lock(mutex);
#ifdef ZZZ_AO_ENABLE_TRACE
    if (pending_depth && pending_depth->IsList(reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native()))) {
        pending_depth->RetireList();
        if (!pending_depth->HasSubmission() || pending_depth->Ready()) {
            pending_depth.reset();
            depth_pending.store(false, std::memory_order_relaxed);
        }
    }
    const auto it = capture.lists.find(cmd);
    if (it != capture.lists.end()) {
        capture.live_rows -= it->second.rows.size();
        capture.lists.erase(it);
    }
#endif
    ao_runtime.lists.erase(cmd);
}

void init_list(a::command_list *cmd)
{
    if (!ao_active.load(std::memory_order_relaxed) || !dx12(cmd->get_device()))
        return;
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    const std::lock_guard lock(mutex);
    if (ao_runtime.swapchain && cmd->get_device() == ao_runtime.swapchain->get_device())
        ao_runtime.lists.try_emplace(cmd);
}

void reset_list(a::command_list *cmd)
{
    forget_list(cmd);
    init_list(cmd);
}

void bundle(a::command_list *parent, a::command_list *child)
{
    on_ao_list(parent, [](AoList &list) {
        list.pixel_shader = 0;
        list.graphics_layout = 0;
        list.pipeline = {};
        list.character = {};
    });
#ifdef ZZZ_AO_ENABLE_TRACE
    on_list(parent, [&](List &list) {
        const auto it = capture.lists.find(child);
        append(list, line("begin_bundle", reinterpret_cast<uintptr_t>(child), it != capture.lists.end()));
        if (it != capture.lists.end() && child != parent) {
            const List &recorded = it->second;
            list.dropped += recorded.dropped;
            list.native_shaders_seen |= recorded.native_shaders_seen;
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
        list.depth_target = {};
        list.color_target_count = 0;
        list.depth_copy_candidate = {};
        list.live_depth_state = a::resource_usage::undefined;
    });
#else
    (void)child;
#endif
}

void submit(a::command_queue *queue, a::command_list *cmd)
{
    bool first_submission = false;
    on_ao_list(cmd, [&](AoList &list) {
        if (list.modified && !ao_runtime.reported) {
            ao_runtime.reported = true;
            first_submission = true;
        }
    });
    if (first_submission)
        log::message(log::level::info, "ZZZ AO submitted Detail AO before upscaling.");

#ifdef ZZZ_AO_ENABLE_TRACE
    if (!capturing.load(std::memory_order_relaxed) && !depth_pending.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    if (pending_depth && pending_depth->IsList(reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native())))
        pending_depth->Submitted(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
    if (capture.device == nullptr || queue->get_device() != capture.device)
        return;
    const auto it = capture.lists.find(cmd);
    append_trace(line("submit", reinterpret_cast<uintptr_t>(queue), static_cast<uint32_t>(queue->get_type()),
                      reinterpret_cast<uintptr_t>(cmd), it != capture.lists.end()));
    if (it == capture.lists.end()) {
        ++capture.unobserved_lists;
        return;
    }
    capture.native_shaders_seen |= it->second.native_shaders_seen;
    for (const auto &row : it->second.rows)
        append_trace(row.text + '\t' + std::to_string(row.repeat));
    if (it->second.dropped)
        append_trace(line("list_truncated", it->second.dropped));
#else
    (void)queue;
#endif
}

#ifdef ZZZ_AO_ENABLE_TRACE
void save_native_shaders(std::ofstream &out, NativeShaderSet observed)
{
    std::array<std::vector<uint8_t>, native_shaders.size()> shaders;
    {
        const std::lock_guard lock(mutex);
        for (size_t i = 0; i < shaders.size(); ++i)
            if (observed.test(i))
                shaders[i] = native_shader_code[i];
    }

    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < shaders.size(); ++i) {
        if (!observed.test(i))
            continue;
        const auto &shader = native_shaders[i];
        std::string hex;
        hex.reserve(shaders[i].size() * 2);
        for (uint8_t byte : shaders[i]) {
            hex += digits[byte >> 4];
            hex += digits[byte & 15];
        }
        out << "native_ao_shader\t" << shader.name << '\t' << shader.fingerprint
            << '\t' << shaders[i].size() << '\t' << hex << '\n';
    }
}

void save(Capture &finished)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("zzz-ao-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(finished.id) + ".tsv");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto &row : finished.rows)
        out << row << '\n';
    save_native_shaders(out, finished.native_shaders_seen);
    std::string depth_rows;
    if (finished.depth) {
        const std::lock_guard lock(mutex);
        depth_rows = finished.depth->ReadRows();
    } else {
        depth_rows = finished.depth_copy_point_seen ? "character_depth_status\tcopy_setup_failed\n"
                                                   : "character_depth_status\tpass_not_matched\n";
    }
    out << depth_rows;
    out << line("summary", finished.presents, finished.unobserved_lists, finished.dropped) << '\n';
    out.close();
    if (out)
        log::message(log::level::info, ("ZZZ AO capture saved: " + path.string()).c_str());
    else
        log::message(log::level::error, "ZZZ AO could not write the capture to the Windows temporary folder.");
}
#endif

void draw_settings(a::effect_runtime *runtime)
{
    if (!dx12(runtime->get_device()))
        return;

    const std::lock_guard lock(mutex);
    if (controls.runtime != runtime)
        controls = {runtime};
    ImGui::Checkbox("Detail AO", &controls.detail_ao);
    ImGui::BeginDisabled(!controls.detail_ao);
    ImGui::Checkbox("Shading View", &controls.shading_view);
    ImGui::EndDisabled();

#ifdef ZZZ_AO_ENABLE_TRACE
    controls.overlay_open = true;
    ImGui::BeginDisabled(controls.capture_requested || capture.swapchain != nullptr);
    if (ImGui::Button(controls.capture_requested ? "Close overlay to capture###capture_trace"
                                                 : "Capture DX12 trace###capture_trace"))
        controls.capture_requested = true;
    ImGui::EndDisabled();
#endif
}

#ifdef ZZZ_AO_ENABLE_TRACE
bool overlay_state_changed(a::effect_runtime *runtime, bool open, a::input_source)
{
    const std::lock_guard lock(mutex);
    if (controls.runtime == runtime)
        controls.overlay_open = open;
    return false;
}
#endif

void destroy_settings_runtime(a::effect_runtime *runtime)
{
    const std::lock_guard lock(mutex);
    if (controls.runtime == runtime) {
        controls = {};
        ao_active.store(false, std::memory_order_relaxed);
        ao_runtime = {};
    }
}

#ifdef ZZZ_AO_ENABLE_TRACE
void poll_depth_readback()
{
    if (!pending_depth)
        return;
    pending_depth->FinishPresent();
    if (pending_depth->IsRetired() && pending_depth->Ready()) {
        pending_depth.reset();
        depth_pending.store(false, std::memory_order_relaxed);
    }
}
#endif

void present(a::command_queue *queue, a::swapchain *swapchain, const a::rect *, const a::rect *,
             uint32_t, const a::rect *)
{
    auto *device = queue->get_device();
    if (!dx12(device))
        return;
    const auto window = static_cast<HWND>(swapchain->get_hwnd());
    const bool focused = window && window == GetForegroundWindow();

#ifdef ZZZ_AO_ENABLE_TRACE
    Capture finished;
#endif
    bool pass_missed = false;
    {
        const std::lock_guard lock(mutex);
#ifdef ZZZ_AO_ENABLE_TRACE
        poll_depth_readback();
#endif
        const bool selected = focused && controls.runtime && controls.runtime->get_hwnd() == window &&
                              controls.runtime->get_device() == device;
        const bool enabled = selected && controls.detail_ao;
        if (enabled && ao_runtime.swapchain != swapchain) {
            ao_runtime = {};
            ao_runtime.swapchain = swapchain;
            ao_runtime.window = window;
            ao_active.store(true, std::memory_order_relaxed);
        } else if (!enabled && ao_runtime.swapchain == swapchain) {
            pass_missed = !ao_runtime.reported;
            ao_runtime = {};
            ao_active.store(false, std::memory_order_relaxed);
        }
        if (enabled)
            ao_runtime.pass = controls.shading_view ? ShadingPass::preview : ShadingPass::composite;
#ifdef ZZZ_AO_ENABLE_TRACE
        if (capture.swapchain == swapchain) {
            ++capture.presents;
            append_trace(line("present", capture.presents, emit_resource(device, swapchain->get_current_back_buffer())));
            const bool depth_in_flight = capture.depth && capture.depth->HasSubmission() &&
                                         !capture.depth->Ready() && capture.presents < max_depth_intervals;
            if (capture.presents >= interval_count && !depth_in_flight) {
                append_trace(line("descriptor_limit_reached", descriptor_limit_reached));
                capturing.store(false, std::memory_order_relaxed);
                finished = std::exchange(capture, Capture{});
            }
        }
        if (selected && controls.capture_requested && !controls.overlay_open &&
            capture.device == nullptr && finished.id == 0) {
            controls.capture_requested = false;
            capture.swapchain = swapchain;
            capture.device = device;
            capture.id = ++next_capture_id;
            append_trace(line("zzz_ao_capture", 1, interval_count));
            append_trace(line("begin", emit_resource(device, swapchain->get_current_back_buffer())));
            capturing.store(true, std::memory_order_relaxed);
        }
#endif
    }
    if (pass_missed)
        log::message(log::level::info, "ZZZ AO did not find a matching pre-DLSS draw.");
#ifdef ZZZ_AO_ENABLE_TRACE
    if (finished.id != 0)
        save(finished);
#endif
}

#ifdef ZZZ_AO_ENABLE_TRACE
void finish_present(a::command_queue *, a::swapchain *)
{
    if (!depth_pending.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(mutex);
    poll_depth_readback();
}
#endif

void destroy_swapchain(a::swapchain *swapchain, bool)
{
#ifdef ZZZ_AO_ENABLE_TRACE
    Capture finished;
#endif
    {
        const std::lock_guard lock(mutex);
        if (ao_runtime.swapchain == swapchain) {
            ao_active.store(false, std::memory_order_relaxed);
            ao_runtime = {};
        }
#ifdef ZZZ_AO_ENABLE_TRACE
        if (capture.swapchain != swapchain)
            return;
        append_trace("swapchain_destroyed");
        capturing.store(false, std::memory_order_relaxed);
        finished = std::exchange(capture, Capture{});
#endif
    }
#ifdef ZZZ_AO_ENABLE_TRACE
    save(finished);
#endif
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

extern "C" __declspec(dllexport) const char *NAME = "ZZZ DX12 AO";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Native form shading and ambient occlusion before DLSS in ZZZ 3.2.";

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
#ifdef ZZZ_AO_ENABLE_TRACE
        observe<addon_event::push_descriptors, root_descriptor>();
#endif
        observe<addon_event::bind_pipeline, bind_pipeline>();
        observe<addon_event::bind_render_targets_and_depth_stencil, bind_targets>();
        observe<addon_event::begin_render_pass, begin_pass>();
        observe<addon_event::end_render_pass, end_pass>();
        observe<addon_event::bind_viewports, viewport>();
        observe<addon_event::barrier, barrier>();
#ifdef ZZZ_AO_ENABLE_TRACE
        observe<addon_event::clear_depth_stencil_view, clear_depth>();
#endif
        observe<addon_event::copy_resource, copy_resource>();
#ifdef ZZZ_AO_ENABLE_TRACE
        observe<addon_event::copy_texture_region, copy_texture>();
#endif
        observe<addon_event::draw, draw>();
        observe<addon_event::draw_indexed, draw_indexed>();
        observe<addon_event::dispatch, dispatch>();
#ifdef ZZZ_AO_ENABLE_TRACE
        observe<addon_event::draw_or_dispatch_indirect, indirect>();
#endif
        observe<addon_event::init_command_list, init_list>();
        observe<addon_event::reset_command_list, reset_list>();
        observe<addon_event::destroy_command_list, forget_list>();
        observe<addon_event::execute_secondary_command_list, bundle>();
        observe<addon_event::execute_command_list, submit>();
        observe<addon_event::present, present>();
#ifdef ZZZ_AO_ENABLE_TRACE
        observe<addon_event::finish_present, finish_present>();
#endif
        observe<addon_event::destroy_swapchain, destroy_swapchain>();
        observe<addon_event::destroy_effect_runtime, destroy_settings_runtime>();
#ifdef ZZZ_AO_ENABLE_TRACE
        observe<addon_event::reshade_open_overlay, overlay_state_changed>();
#endif
        reshade::register_overlay(nullptr, guarded<draw_settings>(draw_settings));
        reshade::log::message(reshade::log::level::info, "ZZZ DX12 AO loaded. Use the Detail AO checkbox in the ReShade Add-ons panel.");
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
