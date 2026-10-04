// Internal bytecode optimisation. Constants keep both value representations so
// folding never discards alpha, masks, or the distinction between raw and RGB.
constexpr int constant_instruction = -1;
struct constant_value { f4 colour, raw; };
static constant_value scalar_value(float x) { return {{x,x,x,1}, {x,x,x,x}}; }
static constant_value select_channel(constant_value value, int channel) {
    if (!channel) return value;
    return scalar_value(channel <= 3 ? value.raw[channel-1] : channel == 4 ? value.colour.w : value.raw.w);
}
static std::optional<constant_value> fold_instruction(
    const instruction& op, const std::array<std::optional<constant_value>, graph_registers>& registers) {
    if (op.code.x == constant_instruction) return constant_value{op.value, op.defaults};
    const auto kind = graph_kind(op.code.x);
    if (kind == graph_kind::texture_sample || kind == graph_kind::texcoord) return {};
    constant_value args[3];
    for (int p = 0; p < 3; ++p) {
        args[p] = scalar_value(op.defaults[p]);
        if (op.inputs[p] < 0) continue;
        if (!registers[op.inputs[p]]) return {};
        args[p] = select_channel(*registers[op.inputs[p]], op.channels[p]);
    }
    constant_value result = scalar_value(0);
    float rgb[3]{};
    for (int c = 0; c < 3; ++c) {
        const float a = args[0].colour[c], b = args[1].colour[c], t = args[2].colour[c];
        float value = a;
        switch (kind) {
        case graph_kind::scalar: value = op.value.x; break;
        case graph_kind::vector: value = op.value[c]; break;
        case graph_kind::add: value = a+b; break;
        case graph_kind::subtract: value = a-b; break;
        case graph_kind::multiply: value = a*b; break;
        case graph_kind::divide: value = std::abs(b) < 1e-8f ? 0 : a/b; break;
        case graph_kind::lerp: value = a*(1-t)+b*t; break;
        case graph_kind::one_minus: value = 1-a; break;
        case graph_kind::saturate: value = std::clamp(a,0.f,1.f); break;
        case graph_kind::power: value = b == 0 ? 1 : std::pow(std::max(0.f,a),b); break;
        default: break;
        }
        rgb[c] = std::isfinite(value) ? value : 0;
    }
    float alpha = args[0].colour.w;
    if (kind == graph_kind::scalar || kind == graph_kind::vector) alpha = 1;
    if (kind == graph_kind::multiply) alpha *= args[1].colour.w;
    if (kind == graph_kind::lerp) alpha = alpha*(1-args[2].colour.x) + args[1].colour.w*args[2].colour.x;
    result.colour = {rgb[0],rgb[1],rgb[2],alpha};
    result.raw = {rgb[0],rgb[1],rgb[2],rgb[0]};
    return result;
}
static i4 optimise_program(std::vector<instruction>& instructions, i4 span) {
    std::array<std::optional<constant_value>, graph_registers> constants;
    std::array<int, graph_registers> definitions;
    definitions.fill(-1);
    std::vector<std::array<int,3>> dependencies(span.y);
    for (int i = 0; i < span.y; ++i) {
        auto& op = instructions[span.x+i];
        auto value = fold_instruction(op, constants);
        constants[op.code.y] = value;
        if (value) {
            op.code = {constant_instruction, op.code.y, -1, 0};
            op.inputs = {-1,-1,-1,0};
            op.channels = {};
            op.value = value->colour;
            op.defaults = value->raw;
        }
        for (int p = 0; p < 3; ++p) dependencies[i][p] = op.inputs[p] < 0 ? -1 : definitions[op.inputs[p]];
        definitions[op.code.y] = i;
    }
    std::vector<bool> used(span.y, false);
    used[definitions[span.z]] = true;
    for (int i = span.y-1; i >= 0; --i) {
        if (used[i]) for (int dependency : dependencies[i]) if (dependency >= 0) used[dependency] = true;
    }
    int count = 0;
    for (int i = 0; i < span.y; ++i) if (used[i]) instructions[span.x+count++] = instructions[span.x+i];
    instructions.resize(span.x+count);
    span.y = count;
    return span;
}
static bool opaque_program(const std::vector<instruction>& instructions, i4 program,
                           bool explicit_mask, const std::function<int(int)>& channels) {
    if (!program.y) return true;
    if (!explicit_mask && program.w != 0) return true;
    std::array<std::optional<float>, graph_registers> alpha;
    for (int i = 0; i < program.y; ++i) {
        const auto& op = instructions[program.x+i];
        if (op.code.x == constant_instruction) {
            alpha[op.code.y] = op.value.w;
            if (explicit_mask && i == program.y-1) {
                return select_channel({op.value,op.defaults}, program.w).colour.x >= 1;
            }
            continue;
        }
        if (explicit_mask) return false;
        if (op.code.x == int(graph_kind::texture_sample)) {
            alpha[op.code.y] = op.code.z < 0 || (channels && channels(op.code.z) == 3)
                                   ? std::optional<float>(1.f) : std::nullopt;
            continue;
        }
        auto input = [&](int p) -> std::optional<float> {
            return op.inputs[p] < 0 || op.channels[p] != 0 ? std::optional<float>(1.f) : alpha[op.inputs[p]];
        };
        auto result = input(0), second = input(1);
        if (op.code.x == int(graph_kind::texcoord)) result = 1.f;
        if (op.code.x == int(graph_kind::multiply)) result = result && second ? std::optional<float>(*result * *second) : std::nullopt;
        if (op.code.x == int(graph_kind::lerp)) result = std::nullopt;
        alpha[op.code.y] = result;
    }
    return !explicit_mask && alpha[program.z] && *alpha[program.z] >= 1;
}
