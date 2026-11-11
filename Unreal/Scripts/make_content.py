# Makes the project's few materials, so the repository holds text rather than binary
# assets. tools\build-unreal.ps1 runs it inside the editor, without a window.
import unreal

FOLDER = '/Game/Teinora'
tools = unreal.AssetToolsHelpers.get_asset_tools()
materials = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary


def begin(name):
    path = f'{FOLDER}/{name}'
    if assets.does_asset_exist(path):
        assets.delete_asset(path)
    return tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


def connect(node, output, target):
    """Connections fail without an error, leaving the input unset, so a failure stops the script."""
    if not materials.connect_material_property(node, output, target):
        raise RuntimeError(f'Could not connect {node.get_name()} to {target}')


def link(source, output, node, name):
    if not materials.connect_material_expressions(source, output, node, name):
        raise RuntimeError(f'Could not connect {source.get_name()} to {name}')


def constant(material, value, x, y):
    node = materials.create_material_expression(material, unreal.MaterialExpressionConstant, x, y)
    node.set_editor_property('r', value)
    return node


def finish(material):
    materials.recompile_material(material)
    assets.save_asset(material.get_path_name())
    unreal.log(f'Made {material.get_name()}')


def custom_node(material, code, names, x, y):
    node = materials.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property('code', code)
    node.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    inputs = []
    for name in names:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        inputs.append(item)
    node.set_editor_property('inputs', inputs)
    return node


def scalar_parameter(material, name, value, x, y):
    node = materials.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property('parameter_name', name)
    node.set_editor_property('default_value', value)
    return node


def vector_parameter(material, name, x, y):
    node = materials.create_material_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
    node.set_editor_property('parameter_name', name)
    return node


def texture_parameter(material, name, x, y):
    node = materials.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter, x, y)
    node.set_editor_property('parameter_name', name)
    node.set_editor_property('texture', unreal.load_asset('/Engine/EngineResources/WhiteSquareTexture'))
    return node


# How the standalone game's Shaders/Surfaces.hlsli picks the ground's surfaces. Positions
# come in centimetres with Y turned round, and go on in that game's metres and axes.
SURFACE_WEIGHTS = '''
float2 Flat = float2(World.x, -World.y) * 0.01;
float Height = World.z * 0.01;
float3 Up = normalize(Normal);
float2 Close = Flat / 8.0;
float2 Large = Flat / 64.0;
float Broad = Texture2DSampleLevel(RockNormal, RockNormalSampler, Large, 3.0).a;
float Waver = Broad - 0.5;
float Above = Tint.b * 24.0 - 8.0; // EncodeGroundLevel, undone
float Rock = 1.0 - smoothstep(0.70, 0.80, Up.z + Waver * 0.08);
float Sand = (1.0 - smoothstep(0.8, 2.0, Above + Waver * 1.6)) * (1.0 - Rock);
float Snow = smoothstep(160.0, 182.0, Height + Waver * 20.0) * (1.0 - Rock) * saturate(1.0 - Sand * 2.0) * smoothstep(0.0, 0.6, Above);
float Grass = saturate(1.0 - Rock - Sand - Snow);
'''

# Water comes in as how far under it this point lies and how much of it is the sea's. The net of light
# on the bed is the standalone game's GetCaustics. What that game does in its water pass, fading the
# colours along the way through, is done here to the bed itself, since the surface above it is drawn by
# rays, which know nothing of the depth behind it. A ray bent by the surface is steep, one that is not
# can be at any angle, so the path is held to what the bent one would be. From inside the water the
# camera's own pass does this, over the whole distance.
SURFACE_COLOR = SURFACE_WEIGHTS + '''
float3 Result = Tint;
if (Kind >= 0.375)
{
    float3 Stone = lerp(Texture2DSample(RockColor, RockColorSampler, Close).rgb, Texture2DSampleLevel(RockColor, RockColorSampler, Large, 5.0).rgb, 0.3);
    if (Kind < 0.75)
    {
        // Steep faces would smear a texture laid on from above, so it is projected along each axis.
        float3 Facing = pow(abs(Up), 4.0);
        Facing /= Facing.x + Facing.y + Facing.z;
        Stone = Stone * Facing.z
            + Texture2DSample(RockColor, RockColorSampler, float2(Flat.y, Height) / 8.0).rgb * Facing.x
            + Texture2DSample(RockColor, RockColorSampler, float2(Flat.x, Height) / 8.0).rgb * Facing.y;
        Result = Kind > 0.62 ? Stone * 0.12 : Stone * Tint * 4.5; // lava's crust is nearly black
    }
    else
    {
        Result = Stone * Rock;
        Result += lerp(Texture2DSample(GrassColor, GrassColorSampler, Close).rgb, Texture2DSampleLevel(GrassColor, GrassColorSampler, Large, 5.0).rgb, 0.3) * Grass;
        Result += lerp(Texture2DSample(SandColor, SandColorSampler, Close).rgb, Texture2DSampleLevel(SandColor, SandColorSampler, Large, 5.0).rgb, 0.3) * Sand;
        Result += lerp(Texture2DSample(SnowColor, SnowColorSampler, Close).rgb, Texture2DSampleLevel(SnowColor, SnowColorSampler, Large, 5.0).rgb, 0.3) * Snow;
        // Near the eye the grass cards give the green, and the soil shows between them, as in Shaders/Surfaces.hlsli.
        float Wide = smoothstep(25.0, 180.0, GetPixelDepth(Parameters) * 0.01);
        float3 Soil = lerp(float3(0.66, 0.5, 0.34), float3(1.0, 1.0, 1.0), Wide);
        Result *= lerp(float3(1.0, 1.0, 1.0), lerp(float3(1.16, 1.0, 0.8), float3(0.82, 1.0, 1.1), Tint.g) * Soil, Grass);
        Result *= (0.8 + 0.4 * Broad) * Tint.r * 2.0;
        Result *= lerp(1.0, 0.6, 1.0 - smoothstep(0.1, 0.9, Above + Waver * 0.5)); // ground the water has been at stays dark
    }
}
else if (Kind >= 0.125)
{
    float2 Round = float2((Flat.x + Flat.y) * 0.8, Height * 0.3) / 8.0;
    Result = Texture2DSample(BarkColor, BarkColorSampler, Round).rgb * Tint * 4.5;
}

float Under = Water.x;
if (Under > 0.0)
{
    float A = Texture2DSampleLevel(SandNormal, SandNormalSampler, Flat / 7.0 + WorldTime * float2(0.024, 0.017), 2.0).a;
    float B = Texture2DSampleLevel(SandNormal, SandNormalSampler, Flat.yx / 4.0 - WorldTime * float2(0.03, 0.021), 2.0).a;
    float Net = saturate(1.0 - abs(A - B) * 7.0);
    Result *= 1.0 + Net * Net * Net * Net * 2.2 * exp(-Under * 0.32) * saturate(Under * 3.0);

    // Foam where the sea runs up the shore. The surface has no idea how shallow it is, but the bed does.
    float Lapping = (1.0 - smoothstep(0.0, 0.14, Under)) * (0.6 + 0.4 * sin(Under * 40.0 - WorldTime * 1.3)) * (0.3 + Net * 1.2) * Water.y;
    Result = lerp(Result, float3(0.8, 0.83, 0.83), saturate(Lapping) * 0.75);

    if (EyeUnder < 0.5)
    {
        float Path = Under / max(abs(normalize(Ray).z), 0.6);
        float3 Passing = exp(-float3(0.36, 0.085, 0.06) * Path);
        float3 Haze = lerp(float3(0.025, 0.15, 0.13), float3(0.02, 0.14, 0.2), Water.y);
        Result = Result * Passing + Haze * (1.0 - Passing);
    }
}
return Result;
'''

SURFACE_NORMAL = SURFACE_WEIGHTS + '''
if (Kind < 0.375)
{
    return Up;
}
float2 Bend = Texture2DSample(RockNormal, RockNormalSampler, Close).xy * 2.0 - 1.0;
if (Kind <= 0.75)
{
    float3 Facing = pow(abs(Up), 4.0);
    Facing /= Facing.x + Facing.y + Facing.z;
    float2 Side = Texture2DSample(RockNormal, RockNormalSampler, float2(Flat.y, Height) / 8.0).xy * 2.0 - 1.0;
    float2 Front = Texture2DSample(RockNormal, RockNormalSampler, float2(Flat.x, Height) / 8.0).xy * 2.0 - 1.0;
    float3 Slope = float3(Bend, 0.0) * Facing.z + float3(0.0, Side) * Facing.x + float3(Front.x, 0.0, Front.y) * Facing.y;
    return normalize(Up + float3(Slope.x, -Slope.y, Slope.z));
}
if (Kind > 0.75)
{
    Bend *= Rock;
    Bend += (Texture2DSample(GrassNormal, GrassNormalSampler, Close).xy * 2.0 - 1.0) * Grass;
    Bend += (Texture2DSample(SandNormal, SandNormalSampler, Close).xy * 2.0 - 1.0) * Sand;
    Bend += (Texture2DSample(SnowNormal, SnowNormalSampler, Close).xy * 2.0 - 1.0) * Snow;
}
return normalize(Up + float3(Bend.x, -Bend.y, 0.0));
'''

SURFACE_TEXTURES = ('SandColor', 'SandNormal', 'GrassColor', 'GrassNormal', 'RockColor', 'RockNormal', 'SnowColor', 'SnowNormal', 'BarkColor')

# The standalone game's GetMoltenGlow in Shaders/Surfaces.hlsli, for what is made of TerrainMolten.
MOLTEN_GLOW = '''
if (Kind < 0.62 || Kind > 0.75)
{
    return float3(0.0, 0.0, 0.0);
}

float2 Flat = float2(World.x, -World.y) * 0.01;
float2 Drift = float2(sin(WorldTime * 0.043), cos(WorldTime * 0.031)) * 0.3;
float Heat = Tint.r;
float2 Places[2] = { Flat * 0.5 + Drift, Flat * 2.0 - Drift * 2.0 };
float Gaps[2];
for (int Which = 0; Which < 2; ++Which)
{
    float2 Cell = floor(Places[Which]);
    float Nearest = 8.0;
    float Second = 8.0;
    for (int Y = -1; Y <= 1; ++Y)
    {
        for (int X = -1; X <= 1; ++X)
        {
            float2 Home = Cell + float2(X, Y);
            float2 Shift = frac(sin(float2(dot(Home, float2(127.1, 311.7)), dot(Home, float2(269.5, 183.3)))) * 43758.5453);
            float Reach = length(Home + Shift - Places[Which]);
            Second = min(Second, max(Reach, Nearest));
            Nearest = min(Nearest, Reach);
        }
    }
    Gaps[Which] = Second - Nearest;
}

float Open = exp(-Gaps[0] * lerp(40.0, 7.0, Heat)) + 0.45 * exp(-Gaps[1] * lerp(40.0, 11.0, Heat));
float Pulse = 0.86 + 0.14 * sin(WorldTime * 0.6 + dot(Flat, float2(0.31, 0.23)));
float Lit = saturate(Open * Pulse * smoothstep(0.0, 0.3, Heat));

float3 Color = lerp(float3(0.9, 0.09, 0.01), float3(1.0, 0.36, 0.03), smoothstep(0.15, 0.6, Lit));
Color = lerp(Color, float3(1.0, 0.72, 0.28), smoothstep(0.65, 1.0, Lit));
return Color * Lit * Lit * 3.0;
'''

# The standalone game's Shaders/Flame.hlsli, line for line: a flame as a volume, the eye's ray traced through
# it. Each flame is a box around its cylinder; World is the point of the box drawn, Eye the camera, Foot the
# flame's foot, all in Unreal's units and axes, turned here into the world's metres with z up.
FLAME_BODY = '''
struct FFlame
{
    float Speck(int3 Cell)
    {
        uint Mixed = uint(Cell.x) * 1597334677u ^ uint(Cell.y) * 3812015801u ^ uint(Cell.z) * 2798796415u;
        Mixed = (Mixed ^ (Mixed >> 16)) * 2654435769u;
        Mixed ^= Mixed >> 15;
        return float(Mixed >> 8) / 16777216.0;
    }

    float Noise(float3 Place)
    {
        float3 Low = floor(Place);
        int3 Cell = int3(Low);
        float3 Part = Place - Low;
        float3 Blend = Part * Part * (3.0 - 2.0 * Part);
        float Near = lerp(lerp(Speck(Cell), Speck(Cell + int3(1, 0, 0)), Blend.x), lerp(Speck(Cell + int3(0, 1, 0)), Speck(Cell + int3(1, 1, 0)), Blend.x), Blend.y);
        float Far = lerp(lerp(Speck(Cell + int3(0, 0, 1)), Speck(Cell + int3(1, 0, 1)), Blend.x), lerp(Speck(Cell + int3(0, 1, 1)), Speck(Cell + int3(1, 1, 1)), Blend.x), Blend.y);
        return lerp(Near, Far, Blend.z);
    }

    float3 Color(float Heat)
    {
        float3 Shade = lerp(float3(0.9, 0.08, 0.01), float3(1.0, 0.36, 0.05), smoothstep(0.05, 0.35, Heat));
        Shade = lerp(Shade, float3(1.0, 0.72, 0.28), smoothstep(0.35, 0.7, Heat));
        return lerp(Shade, float3(1.0, 0.93, 0.78), smoothstep(0.7, 1.0, Heat));
    }

    float4 March(float3 Origin, float3 Direction, float Height, float Breadth, float Seed, float Time, float Jitter)
    {
        float Radius = Height * Breadth * 1.4;
        float Top = Height * 1.15;
        float Enter = 0.0;
        float Leave = 1e9;
        float Level = dot(Direction.xy, Direction.xy);
        float Toward = dot(Origin.xy, Direction.xy);
        float Outside = dot(Origin.xy, Origin.xy) - Radius * Radius;
        if (Level > 1e-6)
        {
            float Square = Toward * Toward - Level * Outside;
            if (Square <= 0.0)
            {
                return float4(0.0, 0.0, 0.0, 0.0);
            }
            float Root = sqrt(Square);
            Enter = max(Enter, (-Toward - Root) / Level);
            Leave = min(Leave, (-Toward + Root) / Level);
        }
        else if (Outside > 0.0)
        {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        if (abs(Direction.z) > 1e-6)
        {
            float Floor = -Origin.z / Direction.z;
            float Ceiling = (Top - Origin.z) / Direction.z;
            Enter = max(Enter, min(Floor, Ceiling));
            Leave = min(Leave, max(Floor, Ceiling));
        }
        else if (Origin.z < 0.0 || Origin.z > Top)
        {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        if (Leave <= Enter)
        {
            return float4(0.0, 0.0, 0.0, 0.0);
        }

        float Clock = fmod(Time, 600.0);
        float Stride = (Leave - Enter) / 20.0;
        float3 Light = float3(0.0, 0.0, 0.0);
        float Thick = 0.0;
        for (int Step = 0; Step < 20; ++Step)
        {
            float3 Here = (Origin + Direction * (Enter + (Step + Jitter) * Stride)) / Height;
            float Up = Here.z;
            float3 Flowing = float3(Here.xy * 3.0, Up * 2.2 - Clock * 2.4) + Seed;
            float2 Sway = (float2(Noise(Flowing * 0.45 + 17.0), Noise(Flowing * 0.45 + 31.0)) - 0.5) * Up * 0.55;
            float Churn = Noise(Flowing) * 0.5 + Noise(Flowing * 2.3 + 5.0) * 0.3 + Noise(Flowing * 5.1 + 9.0) * 0.2;
            float Width = Breadth * pow(saturate(1.0 - Up / 1.15), 0.75) * smoothstep(-0.02, 0.06, Up);
            float Inside = 1.0 - length(Here.xy - Sway) / max(Width, 1e-3);
            float Body = saturate(Inside * 1.4 + (Churn - 0.5) * (0.9 + 1.6 * Up));
            float Heat = saturate(Body * (1.25 - 0.8 * Up) * (0.65 + 0.7 * Churn));
            float Span = Stride / Height;
            Light += Color(Heat) * (Heat * Heat * Span * 4.0);
            Thick += Body * Span;
        }
        return float4(Light * 1.8, 1.0 - exp(-Thick * 4.0)); // 1.8 where the standalone game has 6: Unreal's camera takes more light
    }
};
FFlame Flames;
float3 Here = float3(World.x, -World.y, World.z) * 0.01;
float3 From = float3(Eye.x, -Eye.y, Eye.z) * 0.01;
float3 Base = float3(Foot.x, -Foot.y, Foot.z) * 0.01;
float Jitter = frac(52.9829189 * frac(dot(Pixel.xy, float2(0.06711056, 0.00583715))));
float4 Flame = Flames.March(From - Base, normalize(Here - From), Height, Breadth, Seed, WorldTime, Jitter);
'''

# The standalone game's Shaders/Puffs.hlsl. A sheet comes with its place in UV0, from -1 to 1, in UV1
# how thick the puff is and a number of its own, and in UV2 how much of the light on it it gives back.
PUFF_BODY = '''
float Rise = frac(WorldTime * 0.004) * 60.0;
float2 Moving = Place + Look.y - float2(0.0, Rise);
float2 Spots[6] = { Moving * 1.6, Moving * 3.4 + 7.0, Moving * 7.1 + 3.0, Moving * 1.1 + 13.0, Moving * 1.1 + 29.0, Moving * 14.3 + 1.0 };
float Noise[6];
for (int Index = 0; Index < 6; ++Index)
{
    float2 Cell = floor(Spots[Index]);
    float2 Part = Spots[Index] - Cell;
    float2 Blend = Part * Part * (3.0 - 2.0 * Part);
    int2 Whole = int2(Cell);
    uint4 Keys = uint4(Whole.x, Whole.x + 1, Whole.x, Whole.x + 1) * 1597334677u + uint4(Whole.y, Whole.y, Whole.y + 1, Whole.y + 1) * 3812015801u;
    Keys = (Keys ^ (Keys >> 16)) * 2654435769u;
    float4 Corners = float4(Keys >> 8) / 16777216.0;
    Noise[Index] = lerp(lerp(Corners.x, Corners.y, Blend.x), lerp(Corners.z, Corners.w, Blend.x), Blend.y);
}

float Billows = Noise[0] * 0.5 + Noise[1] * 0.27 + Noise[2] * 0.15 + Noise[5] * 0.08;
float2 Torn = float2(Noise[3], Noise[4]) - 0.5;
float Thin = 1.0 - saturate(Look.x);
float Round = 1.0 - length(Place + Torn * (0.9 + 0.5 * Thin));
float Sheet = saturate((1.0 - length(Place)) * 4.0);
float Body = smoothstep(0.0, 0.3, Round * 1.4 - (1.0 - Billows) * (0.75 + 0.45 * Thin)) * Sheet * (0.6 + 0.4 * Billows);
'''

# The standalone game's ShadeCard in Shaders/Surfaces.hlsli. The cards' own coordinates come in UV0,
# and UV1 says whether a card is leaves, grass blades or needles, at 0, 0.5 and 1, and how light it is.
LEAF_COLOR = '''
float4 Texel = Which.x > 0.75 ? Texture2DSample(NeedleColor, NeedleColorSampler, Place)
    : (Which.x > 0.25 ? Texture2DSample(GrassBladeColor, GrassBladeColorSampler, Place) : Texture2DSample(LeafColor, LeafColorSampler, Place));
float3 Tone = lerp(float3(0.72, 0.72, 0.74), float3(1.3, 1.12, 0.74), Which.y);
float Shown = Which.x > 0.25 && Which.x < 0.75 ? 1.0 : 1.5; // grass stands in the full sun, where the engine's foliage light makes it paler
return lerp(Texel.rgb, dot(Texel.rgb, float3(0.3, 0.6, 0.1)).xxx, 0.22) * Tone * Shown;
'''

# Sampled finer than the pixel asks for, or at a distance a card's holes average away and it turns solid.
LEAF_MASK = '''
return (Which.x > 0.75 ? Texture2DSampleBias(NeedleColor, NeedleColorSampler, Place, -2.0)
    : (Which.x > 0.25 ? Texture2DSampleBias(GrassBladeColor, GrassBladeColorSampler, Place, -2.0) : Texture2DSampleBias(LeafColor, LeafColorSampler, Place, -2.0))).a;
'''


# The standalone game's Shaders/Terrain.hlsl moves its vertices like this. The world's axes and metres
# inside, Unreal's on the way out. The moves come from CollectMoves in Game/Plants.cpp, in two rows of a sheet.
PLANT_SWAY = '''
float3 Move = float3(0.0, 0.0, 0.0);
if (Sway > 0.0)
{
    float3 Here = float3(World.x, -World.y, World.z) * 0.01;
    float Gust = 0.65 + 0.35 * sin(WorldTime * 1.9 + dot(Here.xy, float2(0.21, 0.13)));
    Move.xy = Wind.xy * 0.075 * Sway * Gust;
    if (Kind < 0.19)
    {
        float Flutter = sin(WorldTime * 7.0 + dot(Here, float3(3.1, 2.3, 1.7)));
        Move += float3(0.5, 0.4, 0.3) * Flutter * Sway * 0.035 * (0.4 + length(Wind.xy) * 0.2);
    }

    for (int Index = 0; Index < int(PlantCount); ++Index)
    {
        float4 Moved = PlantMap.Load(int3(Index, 0, 0));
        float3 Away = Here + Move - Moved.xyz;
        float Share = saturate(1.0 - dot(Away, Away) / (Moved.w * Moved.w));
        Move += PlantMap.Load(int3(Index, 1, 0)).xyz * (Share * Share);
    }
}
return float3(Move.x, -Move.y, Move.z) * 100.0;
'''

PLANT_SWAY_INPUTS = ('World', 'Sway', 'Kind', 'Wind', 'WorldTime', 'PlantCount', 'PlantMap')


def connect_sway(material, kind, x, y):
    """Plants lean, shiver and swing back, all in the vertices."""
    sway = custom_node(material, PLANT_SWAY, PLANT_SWAY_INPUTS, x, y)
    link(materials.create_material_expression(material, unreal.MaterialExpressionWorldPosition, x - 400, y), '', sway, 'World')

    color = materials.create_material_expression(material, unreal.MaterialExpressionVertexColor, x - 400, y + 100)
    link(color, 'A', sway, 'Sway')
    link(kind, '', sway, 'Kind')
    link(vector_parameter(material, 'Wind', x - 400, y + 200), '', sway, 'Wind')
    link(scalar_parameter(material, 'WorldTime', 0.0, x - 400, y + 300), '', sway, 'WorldTime')
    link(scalar_parameter(material, 'PlantCount', 0.0, x - 400, y + 400), '', sway, 'PlantCount')
    link(texture_parameter(material, 'PlantMap', x - 400, y + 500), '', sway, 'PlantMap')
    connect(sway, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)


def make_terrain():
    material = begin('M_Terrain')
    material.set_editor_property('tangent_space_normal', False)

    names = ('World', 'Normal', 'Tint', 'Kind') + SURFACE_TEXTURES
    color = custom_node(material, SURFACE_COLOR, names + ('Water', 'Ray', 'EyeUnder', 'WorldTime'), -300, 0)
    normal = custom_node(material, SURFACE_NORMAL, names, -300, 300)

    world = materials.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -800, 0)
    vertex_normal = materials.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -800, 100)
    tint = materials.create_material_expression(material, unreal.MaterialExpressionVertexColor, -800, 200)
    coordinate = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -1000, 300)
    kind = materials.create_material_expression(material, unreal.MaterialExpressionComponentMask, -800, 300)
    kind.set_editor_property('r', True)
    kind.set_editor_property('g', False)
    kind.set_editor_property('b', False)
    kind.set_editor_property('a', False)
    link(coordinate, '', kind, '')

    for node in (color, normal):
        link(world, '', node, 'World')
        link(vertex_normal, '', node, 'Normal')
        link(tint, '', node, 'Tint')
        link(kind, '', node, 'Kind')

    for index, name in enumerate(SURFACE_TEXTURES):
        texture = texture_parameter(material, name, -800, 400 + index * 120)
        link(texture, '', color, name)
        link(texture, '', normal, name)

    water = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, -300)
    water.set_editor_property('coordinate_index', 1)
    link(water, '', color, 'Water')
    link(materials.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS, -800, -200), '', color, 'Ray')
    link(scalar_parameter(material, 'EyeUnder', 0.0, -800, -100), '', color, 'EyeUnder')
    link(scalar_parameter(material, 'WorldTime', 0.0, -1000, -100), '', color, 'WorldTime')

    glow = custom_node(material, MOLTEN_GLOW, ('World', 'Tint', 'Kind', 'WorldTime'), -300, 1400)
    link(world, '', glow, 'World')
    link(tint, '', glow, 'Tint')
    link(kind, '', glow, 'Kind')
    link(scalar_parameter(material, 'WorldTime', 0.0, -1000, -100), '', glow, 'WorldTime')
    connect(glow, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(normal, '', unreal.MaterialProperty.MP_NORMAL)
    connect(constant(material, 0.9, -300, 600), '', unreal.MaterialProperty.MP_ROUGHNESS)
    connect_sway(material, kind, -300, 900)
    finish(material)


def make_bolt():
    # A dithered mask rather than additive, as M_Puff: a ray reflected off the water would stop at the ribbon
    # and show it black wherever it is dim. The mask lets rays through around the glowing middle.
    material = begin('M_Bolt')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('opacity_mask_clip_value', 0.5)
    material.set_editor_property('dither_opacity_mask', True)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('two_sided', True)
    material.set_editor_property('cast_ray_traced_shadows', False)

    # Shaders/Bolts.hlsl. U runs across a ribbon and V says how bright it is.
    code = '''
    float Square = Look.x * Look.x;
    float3 Color = float3(1.0, 0.97, 1.0) * exp(-Square * 14.0) + float3(0.42, 0.34, 1.0) * exp(-Square * 3.0) * 0.16;
    return Color * Look.y * 70.0;
    '''
    node = custom_node(material, code, ('Look',), -300, 0)
    mask = custom_node(material, 'return exp(-Look.x * Look.x * 3.0) * saturate(Look.y * 20.0);', ('Look',), -300, 300)
    mask.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    look = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 0)
    link(look, '', node, 'Look')
    link(look, '', mask, 'Look')
    connect(node, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    connect(mask, '', unreal.MaterialProperty.MP_OPACITY_MASK)
    finish(material)


def make_flame():
    # A dithered mask, as M_Bolt and M_Puff, so that the water's reflections show the flame and not a
    # black sheet. Where only some of it covers what is behind, what does cover is the brighter, so that
    # it adds up to the light the standalone game's flame adds.
    material = begin('M_Flame')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('opacity_mask_clip_value', 0.5)
    material.set_editor_property('dither_opacity_mask', True)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('two_sided', True)
    material.set_editor_property('cast_ray_traced_shadows', False)

    names = ('World', 'Eye', 'Pixel', 'Foot', 'Height', 'Breadth', 'Seed', 'WorldTime')
    color = custom_node(material, FLAME_BODY + 'return Flame.rgb / max(Flame.a, 0.05);', names, -300, 0)
    cover = custom_node(material, FLAME_BODY + 'return Flame.a;', names, -300, 300)
    cover.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    inputs = {
        'World': materials.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -800, 0),
        'Eye': materials.create_material_expression(material, unreal.MaterialExpressionCameraPositionWS, -800, 100),
        'Pixel': materials.create_material_expression(material, unreal.MaterialExpressionScreenPosition, -800, 200),
        'Foot': vector_parameter(material, 'Foot', -800, 300),
        'Height': scalar_parameter(material, 'Height', 1.0, -800, 400),
        'Breadth': scalar_parameter(material, 'Breadth', 0.36, -800, 500),
        'Seed': scalar_parameter(material, 'Seed', 0.0, -800, 600),
        'WorldTime': scalar_parameter(material, 'WorldTime', 0.0, -800, 700),
    }
    for node in (color, cover):
        for name, source in inputs.items():
            link(source, 'PixelPosition' if name == 'Pixel' else '', node, name)
    connect(color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    connect(cover, '', unreal.MaterialProperty.MP_OPACITY_MASK)
    finish(material)


def make_puff():
    # Not translucent: translucency is ray traced here, and a ray reflected off the water stops at the first
    # thing it meets, so the clear part of the sheet would show black in the water. A dithered mask lets
    # rays through where the puff is thin, and the upscaler blends the dither into a see-through cloud.
    material = begin('M_Puff')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('opacity_mask_clip_value', 0.5)
    material.set_editor_property('dither_opacity_mask', True)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('two_sided', True)
    material.set_editor_property('cast_ray_traced_shadows', False)

    shared = ('Place', 'Look', 'WorldTime', 'Glow')
    # Lit from above by the sky and the suns, and from below by the fire under it: FlameLight is the standalone
    # game's flame colour, by the same 0.43 that M_Flame is dimmer by here.
    lit = '''
float Top = Place.y * 0.5 + 0.5;
float3 Above = Light.rgb * (0.55 + 0.3 * Top + 0.5 * Billows);
float3 Below = float3(2.6, 1.1, 0.3) * 0.43 * Shade.y * (0.5 - 0.25 * Top);
return (Above + Below) * Shade.x;
'''
    # A firefly, as in the standalone game's Shaders/Puffs.hlsl: a bright core in a soft round halo, all its own light
    # in the colour of light magic. A dithered mask cannot fade, so the halo is the faint ring the mask keeps.
    firefly_color = '''
if (Glow.x > 0.0)
{
    float Away = length(Place);
    float Core = exp(-Away * Away * 18.0);
    return float3(1.0, 0.95, 0.8) * Glow.x * 6.0 * (Core * 2.0 + 0.2);
}
'''
    firefly_mask = '''
if (Glow.x > 0.0)
{
    return saturate(1.0 - length(Place) * 1.6) * 2.0;
}
'''
    color = custom_node(material, firefly_color + PUFF_BODY + lit, shared + ('Light', 'Shade'), -300, 0)
    thickness = custom_node(material, firefly_mask + PUFF_BODY + 'return Body * Look.x;', shared, -300, 300)
    thickness.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)

    place = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 0)
    look = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 100)
    look.set_editor_property('coordinate_index', 1)
    time = scalar_parameter(material, 'WorldTime', 0.0, -800, 200)
    glow = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 500)
    glow.set_editor_property('coordinate_index', 3)
    for node in (color, thickness):
        link(place, '', node, 'Place')
        link(look, '', node, 'Look')
        link(time, '', node, 'WorldTime')
        link(glow, '', node, 'Glow')
    link(vector_parameter(material, 'Light', -800, 300), '', color, 'Light')
    shade = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 400)
    shade.set_editor_property('coordinate_index', 2)
    link(shade, '', color, 'Shade')

    connect(color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    connect(thickness, '', unreal.MaterialProperty.MP_OPACITY_MASK)
    finish(material)


def make_leaf():
    material = begin('M_Leaf')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('two_sided', True)
    material.set_editor_property('opacity_mask_clip_value', 0.42)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE) # sunlight comes through them

    names = ('Place', 'Which', 'LeafColor', 'NeedleColor', 'GrassBladeColor')
    color = custom_node(material, LEAF_COLOR, names, -300, 0)
    mask = custom_node(material, LEAF_MASK, names, -300, 300)
    mask.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)

    place = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 0)
    which = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 100)
    which.set_editor_property('coordinate_index', 1)
    leaf = texture_parameter(material, 'LeafColor', -800, 200)
    needle = texture_parameter(material, 'NeedleColor', -800, 320)
    blades = texture_parameter(material, 'GrassBladeColor', -800, 440)
    for node in (color, mask):
        link(place, '', node, 'Place')
        link(which, '', node, 'Which')
        link(leaf, '', node, 'LeafColor')
        link(needle, '', node, 'NeedleColor')
        link(blades, '', node, 'GrassBladeColor')

    # A leaf lets through part of what it reflects. It shows against a sun, but not against the planet's faint light.
    through = custom_node(material, 'return Leaf * LeafThrough;', ('Leaf', 'LeafThrough'), -300, 150)
    link(color, '', through, 'Leaf')
    link(scalar_parameter(material, 'LeafThrough', 1.0, -800, 450), '', through, 'LeafThrough')

    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(through, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    connect(mask, '', unreal.MaterialProperty.MP_OPACITY_MASK)
    connect(constant(material, 0.75, -300, 500), '', unreal.MaterialProperty.MP_ROUGHNESS)
    connect_sway(material, constant(material, 0.0, -800, 900), -300, 900) # no bark here: all of it shivers
    finish(material)


# A body fades in water as the bed does, see SURFACE_COLOR. Each instance is told the level of its water.
BODY_COLOR = '''
float Under = WaterLevel - World.z * 0.01;
if (Under <= 0.0 || EyeUnder > 0.5)
{
    return Tint;
}
float Path = Under / max(abs(normalize(Ray).z), 0.6);
float3 Passing = exp(-float3(0.36, 0.085, 0.06) * Path);
return Tint * Passing + float3(0.02, 0.14, 0.16) * (1.0 - Passing);
'''


def make_body():
    material = begin('M_Body')
    material.set_editor_property('used_with_instanced_static_meshes', True)

    color = custom_node(material, BODY_COLOR, ('Tint', 'World', 'Ray', 'WaterLevel', 'EyeUnder'), -300, 0)
    tint = materials.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -800, 0)
    tint.set_editor_property('parameter_name', 'Tint')
    tint.set_editor_property('default_value', unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    link(tint, '', color, 'Tint')
    link(materials.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -800, 150), '', color, 'World')
    link(materials.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS, -800, 250), '', color, 'Ray')

    level = materials.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData, -800, 350)
    level.set_editor_property('data_index', 0)
    link(level, '', color, 'WaterLevel')
    link(scalar_parameter(material, 'EyeUnder', 0.0, -800, 450), '', color, 'EyeUnder')

    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(constant(material, 0.7, -300, 300), '', unreal.MaterialProperty.MP_ROUGHNESS)
    finish(material)


# The surface of Shaders/Water.hlsl as Unreal's ray traced translucency wants it. That pass weighs a
# surface's reflection by its opacity and what lies behind by the rest, and bends the ray by the index of
# refraction. So the surface is a white mirror that is only as opaque as Fresnel says it reflects, and
# the rays find the bed, the sky through the round window from below, and the mirror past its edge by
# themselves. A normal in world space does not turn round for the back of a surface, so it is done here.
# Rays have no pixel to work out a mip from, so distance picks it. Beyond the ground there is no bed to
# see, and the surface turns to plain water.
WATER_READ = '''
float2 Flat = float2(World.x, -World.y) * 0.01;
float Far = distance(World, EyeAt) * 0.01;
float2 Place = (Flat - LiquidArea.xy) * LiquidArea.z;
float Foam = 0.0;
if (all(Place > 0.0) && all(Place < 1.0))
{
    Foam = saturate(Texture2DSampleLevel(LiquidMap, LiquidMapSampler, Place, 0.0).g * 1.4);
}
float Beyond = smoothstep(BedReach * 0.75, BedReach * 0.95, Far);
'''

WATER_NORMAL = WATER_READ + '''
float NearLevel = clamp(log2(Far * 0.085 + 0.001) + 1.0, 0.0, 9.0);
float FineLevel = clamp(log2(Far * 0.34 + 0.001) + 1.0, 0.0, 9.0);
float2 RippleA = Texture2DSampleLevel(SnowNormal, SnowNormalSampler, (Flat + Drift.xy * 0.35) / 16.0, NearLevel).xy * 2.0 - 1.0;
float2 RippleB = Texture2DSampleLevel(SnowNormal, SnowNormalSampler, (Flat.yx - Drift.yx * 0.2) / 4.0, FineLevel).xy * 2.0 - 1.0;
float2 Slope = (RippleA + RippleB) * lerp(0.05, 0.5, WaveShare);
if (all(Place > 0.0) && all(Place < 1.0))
{
    float Step = LiquidArea.z * 0.75;
    float Left = Texture2DSampleLevel(LiquidMap, LiquidMapSampler, Place - float2(Step, 0.0), 0.0).r;
    float Right = Texture2DSampleLevel(LiquidMap, LiquidMapSampler, Place + float2(Step, 0.0), 0.0).r;
    float Down = Texture2DSampleLevel(LiquidMap, LiquidMapSampler, Place - float2(0.0, Step), 0.0).r;
    float Up = Texture2DSampleLevel(LiquidMap, LiquidMapSampler, Place + float2(0.0, Step), 0.0).r;
    Slope -= float2(Right - Left, Up - Down) / 1.5 * 9.0;
}

float3 Seen = normalize(normalize(Swell) + float3(Slope.x, -Slope.y, 0.0)) * Parameters.TwoSidedSign;
if (Parameters.TwoSidedSign > 0.0)
{
    // Where a wave turns a reflection into the water, it bounces off the next one back up.
    float3 Bounce = reflect(-Parameters.CameraVector, Seen);
    if (Bounce.z < 0.01)
    {
        Bounce.z = max(abs(Bounce.z), 0.01);
        Seen = normalize(Parameters.CameraVector + normalize(Bounce));
    }
}
return Seen;
'''

WATER_OPACITY = WATER_READ + '''
// The magic view sees no reflections, only the water: the mirror fades out of it.
float Fresnel = (0.02 + 0.98 * pow(1.0 - saturate(dot(Normal, Parameters.CameraVector)), 5.0)) * (1.0 - MagicView);
return saturate(max(lerp(Fresnel, 1.0, Foam * 0.6), Beyond));
'''

WATER_COLOR = WATER_READ + '''
float3 Body = lerp(float3(0.025, 0.15, 0.13), float3(0.02, 0.14, 0.2), WaveShare) * 0.5;
return lerp(lerp(float3(1.0, 1.0, 1.0), Body, Beyond), float3(0.75, 0.8, 0.8), Foam);
'''

WATER_METAL = WATER_READ + '''
return (1.0 - Foam) * (1.0 - Beyond);
'''

WATER_ROUGHNESS = WATER_READ + '''
return lerp(0.05, 0.6, Foam);
'''

# Rays reflected low across the water run into the next wave. Translucency stops at that hit with no
# reflection of its own, which would show black, so the wave gives back the low sky it hides. Only a
# reflection can meet the top of the water on its way up.
WATER_GLOW = '''
return Parameters.TwoSidedSign > 0.0 && Parameters.CameraVector.z < 0.0 ? SkyGlow : float3(0.0, 0.0, 0.0);
'''


def make_water():
    material = begin('M_Water')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property('translucency_lighting_mode', unreal.TranslucencyLightingMode.TLM_SURFACE)
    material.set_editor_property('two_sided', True) # it is seen from under the water as well
    material.set_editor_property('tangent_space_normal', False)
    material.set_editor_property('refraction_method', unreal.RefractionMode.RM_INDEX_OF_REFRACTION)

    world = materials.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -1400, 0)
    eye = vector_parameter(material, 'EyeAt', -1400, 100)
    area = vector_parameter(material, 'LiquidArea', -1400, 200)
    reach = scalar_parameter(material, 'BedReach', 2000.0, -1400, 300)
    liquid = texture_parameter(material, 'LiquidMap', -1400, 400)
    share = scalar_parameter(material, 'WaveShare', 0.0, -1400, 500)

    read = ('World', 'EyeAt', 'LiquidArea', 'BedReach', 'LiquidMap')
    normal = custom_node(material, WATER_NORMAL, read + ('Swell', 'Drift', 'WaveShare', 'SnowNormal'), -800, 0)
    opacity = custom_node(material, WATER_OPACITY, read + ('Normal', 'MagicView'), -300, 300)
    color = custom_node(material, WATER_COLOR, read + ('WaveShare',), -300, 0)
    metal = custom_node(material, WATER_METAL, read, -300, 600)
    roughness = custom_node(material, WATER_ROUGHNESS, read, -300, 900)
    for node in (opacity, metal, roughness):
        node.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)

    for node in (normal, opacity, color, metal, roughness):
        link(world, '', node, 'World')
        link(eye, '', node, 'EyeAt')
        link(area, '', node, 'LiquidArea')
        link(reach, '', node, 'BedReach')
        link(liquid, '', node, 'LiquidMap')

    link(materials.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -1400, 600), '', normal, 'Swell')
    link(vector_parameter(material, 'Drift', -1400, 700), '', normal, 'Drift')
    link(share, '', normal, 'WaveShare')
    link(share, '', color, 'WaveShare')
    link(texture_parameter(material, 'SnowNormal', -1400, 800), '', normal, 'SnowNormal')
    link(normal, '', opacity, 'Normal')
    link(scalar_parameter(material, 'MagicView', 0.0, -800, 400), '', opacity, 'MagicView')

    glow = custom_node(material, WATER_GLOW, ('SkyGlow',), -300, 1500)
    link(vector_parameter(material, 'SkyGlow', -800, 1500), '', glow, 'SkyGlow')
    connect(glow, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(metal, '', unreal.MaterialProperty.MP_METALLIC)
    connect(roughness, '', unreal.MaterialProperty.MP_ROUGHNESS)
    connect(opacity, '', unreal.MaterialProperty.MP_OPACITY)
    connect(normal, '', unreal.MaterialProperty.MP_NORMAL)
    connect(constant(material, 1.33, -300, 1200), '', unreal.MaterialProperty.MP_REFRACTION)
    finish(material)


# The standalone game's Shaders/Underwater.hlsl: the water between the eye and whatever it looks at. The
# way up ends at the surface, which is not in the depth buffer, and what lies past it the rays have drawn.
UNDERWATER_CODE = '''
float2 ViewUv = GetViewportUV(Parameters);
float3 Plain = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv, 14), 14, false).rgb * View.OneOverPreExposure;

float3 Direction = normalize(ViewForward.xyz + ViewRight.xyz * (ViewUv.x * 2.0 - 1.0) + ViewUp.xyz * (1.0 - ViewUv.y * 2.0));
float Distance = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv, 1), 1, false).r * 0.01 / max(dot(Direction, ViewForward.xyz), 0.05);
if (Direction.z > 0.001)
{
    Distance = min(Distance, WaterAbove / Direction.z);
}

float3 Passing = exp(-float3(0.32, 0.075, 0.055) * Distance);
return Plain * Passing + WaterHaze * (1.0 - Passing);
'''


def make_underwater():
    material = begin('M_Underwater')
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property('blendable_location', unreal.BlendableLocation.BL_SCENE_COLOR_BEFORE_BLOOM)

    vectors = ('WaterHaze', 'ViewForward', 'ViewRight', 'ViewUp')
    node = custom_node(material, UNDERWATER_CODE, ('WaterAbove',) + vectors + ('Scene', 'Depth'), -300, 0)

    link(scalar_parameter(material, 'WaterAbove', 1.0, -700, -200), '', node, 'WaterAbove')
    row = -100
    for name in vectors:
        link(vector_parameter(material, name, -700, row), '', node, name)
        row += 100

    # Only here so the material asks for these textures: the code reads them by itself.
    scene = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, row)
    scene.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
    depth = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, row + 100)
    depth.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_SCENE_DEPTH)
    link(scene, 'Color', node, 'Scene')
    link(depth, 'Color', node, 'Depth')

    connect(node, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material)


# The standalone game's eye focus: sharp around the point of gaze, blurred away from it.
FOCUS_CODE = '''
float2 ViewUv = GetViewportUV(Parameters);
float Aspect = View.ViewSizeAndInvSize.x * View.ViewSizeAndInvSize.w;
float3 Sharp = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv, 14), 14, false).rgb;

// How far a point lies from the edge of focus towards the edge of the screen, along the line from the
// point of gaze, and a blur that grows as the square of that: hardly there at first, strong at the edge.
float2 Away = ViewUv - Center.xy;
float Distance = length(Away * float2(Aspect, 1.0));
float2 Room = float2(Away.x >= 0.0 ? 1.0 - Center.x : Center.x, Away.y >= 0.0 ? 1.0 - Center.y : Center.y);
float2 Ratio = Room / max(abs(Away), 1e-5);
float ToEdge = Distance * max(min(Ratio.x, Ratio.y), 1.0);
float Toward = saturate((Distance - Radius) / max(ToEdge - Radius, 1e-3));
float Blur = Toward * Toward * Strength;
if (Blur <= 0.0)
{
    return Sharp;
}
float3 Sum = Sharp;
float Total = 1.0;
for (int Index = 1; Index <= 32; ++Index)
{
    float Share = Index / 32.0;
    float Weight = exp(-2.5 * Share);
    float Angle = Index * 2.39996;
    float2 Offset = float2(cos(Angle) / Aspect, sin(Angle)) * sqrt(Share) * 0.02 * Blur;
    Sum += SceneTextureLookup(ViewportUVToSceneTextureUV(saturate(ViewUv + Offset), 14), 14, true).rgb * Weight;
    Total += Weight;
}
return lerp(Sharp, Sum / Total, saturate(Blur * 3.0)) * (1.0 - 0.05 * Blur);
'''


def make_focus():
    material = begin('M_Focus')
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property('blendable_location', unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)

    custom = materials.create_material_expression(material, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property('code', FOCUS_CODE)
    custom.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)

    inputs = []
    for name in ('Center', 'Radius', 'Strength', 'Scene'):
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        inputs.append(item)
    custom.set_editor_property('inputs', inputs)

    center = materials.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -700, -100)
    center.set_editor_property('parameter_name', 'FocusCenter')
    center.set_editor_property('default_value', unreal.LinearColor(0.5, 0.5, 0.0, 1.0))
    radius = materials.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 100)
    radius.set_editor_property('parameter_name', 'FocusRadius')
    radius.set_editor_property('default_value', 0.23)
    strength = materials.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 200)
    strength.set_editor_property('parameter_name', 'FocusStrength')
    strength.set_editor_property('default_value', 0.75)

    # Only here so the material asks for scene textures: the code reads them by itself.
    scene = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, 300)
    scene.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)

    materials.connect_material_expressions(center, '', custom, 'Center')
    materials.connect_material_expressions(radius, '', custom, 'Radius')
    materials.connect_material_expressions(strength, '', custom, 'Strength')
    materials.connect_material_expressions(scene, 'Color', custom, 'Scene')
    connect(custom, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material)


# A sheet of rain streaks over the view, as Shaders/Tonemap.hlsl draws it. Runs after the scene's tonemap, in sRGB.
RAIN_CODE = '''
float2 Uv = GetViewportUV(Parameters);
float3 Color = Scene.rgb;
if (Rain <= 0.001)
{
    return Color;
}
struct Drops
{
    uint Hash(int3 Cell)
    {
        uint Mixed = uint(Cell.x) * 1597334677u ^ uint(Cell.y) * 3812015801u ^ uint(Cell.z) * 2246822519u;
        Mixed ^= Mixed >> 15;
        Mixed *= 2654435769u;
        Mixed ^= Mixed >> 13;
        return Mixed;
    }
};
Drops Library;

// As GetRain in Shaders/Tonemap.hlsl: sheets of drops at a few distances, each measured in metres where it is.
float2 Size = View.ViewSizeAndInvSize.xy;
float Aspect = Size.x / Size.y;
float Behind = SceneTextureLookup(ViewportUVToSceneTextureUV(Uv, 1), 1, false).r * 0.01; // metres to what is seen
Color = lerp(Color, dot(Color, float3(0.333, 0.333, 0.333)).xxx, 0.25 * Rain) * (1.0 - 0.2 * Rain);
float Cover = 0.0;
for (int Layer = 0; Layer < 8; ++Layer)
{
    float Distance = 0.9 * pow(1.5, float(Layer));
    float InFront = saturate((Behind - Distance) / (0.25 * Distance));
    float Tall = 1.2256 * Distance; // 2 tan(31.5°), Gaze::VerticalFov
    float Pixel = Tall / Size.y;
    float2 Place = float2((Uv.x - 0.5) * Aspect, Uv.y - 0.5) * Tall;
    Place.x -= Place.y * Slant;
    Place.y -= RainTime * 9.0;
    float Column = floor(Place.x / 0.07);
    uint Shift = Library.Hash(int3(int(Column), Layer, 7));
    float Down = Place.y / 1.1 + float(Shift & 65535u) / 65535.0 * 7.0;
    uint Drop = Library.Hash(int3(int(Column), int(floor(Down)), Layer));
    if (InFront <= 0.0 || float(Drop & 65535u) / 65535.0 > Rain * (0.7 - 0.05 * float(Layer)))
    {
        continue;
    }
    uint Look = Library.Hash(int3(int(Drop), Layer, 11));
    float Length = 9.0 * 0.025 * (0.8 + 0.5 * float(Look & 65535u) / 65535.0);
    float Middle = (0.2 + 0.6 * float((Look >> 16) & 65535u) / 65535.0) * 0.07;
    float Top = float((Drop >> 16) & 65535u) / 65535.0 * (1.1 - Length);
    float Along = (frac(Down) * 1.1 - Top) / Length;
    if (Along < 0.0 || Along > 1.0)
    {
        continue;
    }
    float Width = max(0.0025 + 0.0025 * 2.5 / Distance, Pixel * 1.3);
    float Across = (frac(Place.x / 0.07) * 0.07 - Middle) / Width;
    float Streak = exp(-Across * Across * 4.0) * smoothstep(0.0, 0.5, Along) * smoothstep(0.0, 0.06, 1.0 - Along) * saturate(0.0025 * 1.6 / Width);
    Cover = 1.0 - (1.0 - Cover) * (1.0 - Streak * InFront);
}
if (Cover > 0.002)
{
    // A drop is a little lens: it shows a bent glimpse of what is behind it, lighter, and of the sky over it.
    float3 Bent = SceneTextureLookup(ViewportUVToSceneTextureUV(Uv + float2(0.0, -0.012), 14), 14, true).rgb;
    Color = lerp(Color, Bent * 1.3 + 0.03, Cover * 0.55);
}
return Color;
'''


# The standalone game's Shaders/Magic.hlsl and the magic part of its Tonemap.hlsl, in one
# pass. The boxes of magic arrive as sheets of slices, and the rays run in that game's axes.
MAGIC_CODE = '''
struct Sheets
{
    float4 Read(Texture2D Sheet, SamplerState Filter, float3 Uvw, float Columns, float Rows, float Slices)
    {
        float Slice = clamp(Uvw.z * Slices - 0.5, 0.0, Slices - 1.001);
        float First = floor(Slice);
        float Next = First + 1.0;
        float2 Inside = clamp(Uvw.xy, 0.012, 0.988);
        float2 Shape = float2(Columns, Rows);
        float2 A = (float2(fmod(First, Columns), floor(First / Columns)) + Inside) / Shape;
        float2 B = (float2(fmod(Next, Columns), floor(Next / Columns)) + Inside) / Shape;
        float4 Texel = lerp(Sheet.SampleLevel(Filter, A, 0.0), Sheet.SampleLevel(Filter, B, 0.0), Slice - First);
        float Density = Texel.r * Texel.r * 120.0; // MagicVolume::DensityScale
        return float4(Density, Texel.gba * Density);
    }
};
Sheets Library;

float2 ViewUv = GetViewportUV(Parameters);
float3 Plain = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv, 14), 14, false).rgb;
if (MagicView <= 0.001 && MagicBends <= 0.0)
{
    return Plain;
}

float3 Direction = normalize(ViewForward.xyz + ViewRight.xyz * (ViewUv.x * 2.0 - 1.0) + ViewUp.xyz * (1.0 - ViewUv.y * 2.0));
float SceneDistance = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv, 1), 1, false).r * 0.01 / max(dot(Direction, ViewForward.xyz), 0.01);
// The magic view sees 100 m, Sim::MagicSightRadius, right through whatever is in the way, and what is inside it.
// Light that dense magic bends in plain sight, though, is only seen as far as what is in the way.
float Limit = MagicBends > 0.0 ? 3500.0 : 100.0;

float Near = 1.5;
float Clear = 1.0;
float3 Glow = float3(0.0, 0.0, 0.0);
float Dense = 0.0;
for (int Index = 0; Index < 44 && Near < Limit; ++Index)
{
    float Far = min(Near * 1.19, Limit);
    float Span = Far - Near;
    float3 Relative = Direction * (0.5 * (Near + Far));

    float3 NearUvw = (Relative - NearCorner.xyz) / NearSize.xyz;
    float3 FromEdge = min(NearUvw, 1.0 - NearUvw);
    float Inside = saturate(min(FromEdge.x, min(FromEdge.y, FromEdge.z)) * 14.0);

    float4 Sampled = float4(0.0, 0.0, 0.0, 0.0);
    if (Inside > 0.0)
    {
        Sampled = Library.Read(NearAtlas, NearAtlasSampler, NearUvw, 8.0, 6.0, 48.0) * Inside;
    }
    if (Inside < 1.0)
    {
        float3 FarUvw = (Relative - FarCorner.xyz) / FarSize.xyz;
        if (all(FarUvw >= 0.0) && all(FarUvw <= 1.0))
        {
            Sampled += Library.Read(FarAtlas, FarAtlasSampler, FarUvw, 4.0, 3.0, 12.0) * (1.0 - Inside);
        }
    }

    float Density = Sampled.x;
    if (Density > 0.0001)
    {
        // As Hiding in Shaders/Magic.hlsl, and bending from the dew point, Sim::MagicDewPoint, up.
        float Taken = (1.0 - exp(-Density * 0.0035 / (1.0 + Density / 20.0) * Span)) * (1.0 - smoothstep(70.0, 100.0, Near));
        Glow += Sampled.yzw / Density * Clear * Taken;
        Clear *= 1.0 - Taken;
        Dense += Near < SceneDistance ? saturate((Density - 30.0) / 13.6) * Span : 0.0;
    }
    Near = Far;
}

// The magic held inside things glows through them, as in the standalone game's Magic.hlsl.
float4 GlowPoints[8] = { GlowPoint0, GlowPoint1, GlowPoint2, GlowPoint3, GlowPoint4, GlowPoint5, GlowPoint6, GlowPoint7 };
float3 GlowColors[8] = { GlowColor0, GlowColor1, GlowColor2, GlowColor3, GlowColor4, GlowColor5, GlowColor6, GlowColor7 };
for (int Held = 0; Held < 8; ++Held)
{
    float Radius = GlowPoints[Held].w;
    float Along = dot(GlowPoints[Held].xyz, Direction);
    float Off = dot(GlowPoints[Held].xyz, GlowPoints[Held].xyz) - Along * Along;
    if (Radius > 0.0 && Along > 0.0 && Along < 100.0 && Off < Radius * Radius)
    {
        float Through = 1.0 - Off / (Radius * Radius);
        Glow += GlowColors[Held].rgb * Through * 2.5;
    }
}

float3 Color = Plain;
float Shimmer = saturate(Dense * 0.03) * MagicBends;
if (Shimmer > 0.0)
{
    float2 Wave = ViewUv * 18.0;
    float2 Offset = float2(sin(Wave.y * 1.7 + sin(Wave.x * 2.3 + MagicTime) * 2.0 + MagicTime * 1.3),
        sin(Wave.x * 1.9 + sin(Wave.y * 2.1 - MagicTime) * 2.0 - MagicTime * 1.1));
    Color = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv + Offset * 0.006 * Shimmer, 14), 14, true).rgb;
}

float3 Linear = pow(max(Color, 0.0), 2.2);
// The world goes grey and dim, and the magic glows in it by its own light, lighting nothing, as in the standalone game's Tonemap.hlsl.
float Grey = dot(Linear, float3(0.2126, 0.7152, 0.0722)) * 0.3;
Linear = lerp(Linear, float3(Grey, Grey, Grey) * (1.0 - 0.5 * (1.0 - Clear)) + Glow * 0.9, MagicView);
return pow(max(Linear, 0.0), 1.0 / 2.2);
'''


# The whole sky behind the clouds, on a dome: Unreal's air and its sun discs, with the standalone
# game's stars (Shaders/Stars.hlsl) and planet (Shaders/Planet.hlsli) seen through them. Directions
# run in that game's axes. The stars come from a sheet made by MakeStarSheet in Game/StarMap.cpp.
SKY_CODE = '''
struct Shapes
{
    float2 Fold(float3 Way)
    {
        float2 Flat = Way.xy / (abs(Way.x) + abs(Way.y) + abs(Way.z));
        if (Way.z < 0.0)
        {
            Flat = (1.0 - abs(Flat.yx)) * float2(Flat.x >= 0.0 ? 1.0 : -1.0, Flat.y >= 0.0 ? 1.0 : -1.0);
        }
        return Flat * 0.5 + 0.5;
    }

    float3 Unfold(float2 Uv)
    {
        float2 Flat = Uv * 2.0 - 1.0;
        float Rise = 1.0 - abs(Flat.x) - abs(Flat.y);
        if (Rise < 0.0)
        {
            Flat = (1.0 - abs(Flat.yx)) * float2(Flat.x >= 0.0 ? 1.0 : -1.0, Flat.y >= 0.0 ? 1.0 : -1.0);
        }
        return normalize(float3(Flat, Rise));
    }

    float RingDensity(float Radius)
    {
        float Edges = smoothstep(1.38, 1.43, Radius) * (1.0 - smoothstep(2.41, 2.5, Radius));
        float Bands = 0.62 + 0.24 * sin(Radius * 41.0) + 0.14 * sin(Radius * 97.0 + 1.3);
        float Gap = 1.0 - 0.9 * (1.0 - smoothstep(0.0, 0.045, abs(Radius - 2.0)));
        return saturate(Bands) * Edges * Gap;
    }

    float RingLight(float3 Spot, float3 Sun)
    {
        float Along = dot(Spot, Sun);
        return Along > 0.0 ? 1.0 : smoothstep(0.96, 1.04, length(Spot - Sun * Along));
    }

    float RingShade(float3 Surface, float3 Sun, float3 Axis)
    {
        float Across = dot(Sun, Axis);
        float Rise = -dot(Surface, Axis) / (abs(Across) > 1e-3 ? Across : 1e-3);
        return Rise > 0.0 ? 1.0 - 0.6 * RingDensity(length(Surface + Sun * Rise)) : 1.0;
    }

    // A sun's corona, as GetCorona in the standalone game's Sky.hlsl: faint, so only seen once the
    // planet covers the disc, with smooth streamers along the sun's equator and a red rim.
    float3 Corona(float3 Way, float3 Sun, float Radius, float3 Light, float Seed, float3 Pole)
    {
        float Strength = max(max(Light.r, Light.g), Light.b);
        float Cosine = dot(Way, Sun);
        if (Strength <= 0.0 || Cosine < cos(Radius * 20.0))
        {
            return float3(0.0, 0.0, 0.0);
        }
        float Out = acos(saturate(Cosine)) / Radius;
        if (Out < 1.0)
        {
            return float3(0.0, 0.0, 0.0);
        }
        float3 Across = normalize(cross(Pole, Sun));
        float3 Up = cross(Sun, Across);
        float3 Off = Way - Sun * Cosine;
        float Angle = atan2(dot(Off, Up), dot(Off, Across));
        float Equator = cos(Angle) * cos(Angle);
        float Falloff = lerp(3.0, 1.8, Equator);
        float Streamers = 0.55 + 0.9 * Equator * (0.6 + 0.4 * sin(Angle * 3.0 + Seed));
        float3 Pearl = lerp(float3(1.0, 1.0, 1.0), Light / Strength, 0.3);
        float3 Glow = Pearl * (Strength * 0.3 * pow(Out, -Falloff) * Streamers);
        float Rim = 1.0 - smoothstep(1.0, 1.035, Out);
        float Prominence = saturate(sin(Angle * 5.0 + Seed * 3.1) * sin(Angle * 11.0 - Seed * 1.7) * 2.5 - 0.9) * (1.0 - smoothstep(1.0, 1.12, Out));
        return Glow + float3(1.0, 0.16, 0.24) * (Strength * 0.3 * 2.5 * max(Rim, Prominence));
    }
};
Shapes Library;

const float StarLight = 2.0e-8; // the brightness of the faintest star over a steradian
const float CellSide = 640.0;
const float Unit = 2.0 / CellSide; // the least angle from a cell to the edge of the nine around it

float3 Way = float3(-Ray.x, Ray.y, -Ray.z);
float Above = smoothstep(-0.002, 0.004, Way.z);
float Mass = sqrt(Way.z * Way.z * 1600.0 + 81.0) - Way.z * 40.0;
float3 Through = exp(-float3(0.2, 0.3, 0.46) * (Mass - 1.0)); // blue goes first through the long path of air

float3 Space = float3(0.0, 0.0, 0.0);
if (Night > 0.0 && Above > 0.0)
{
    float Cosine = cos(StarTurn);
    float Sine = sin(StarTurn);
    float3 FromMiddle = normalize(Way - SkyShift); // the stars lie on the sky's sphere, seen from the eye, see SeenFrom in SceneTypes.h
    float3 Local = FromMiddle * Cosine - cross(StarPole, FromMiddle) * Sine + StarPole * dot(StarPole, FromMiddle) * (1.0 - Cosine);
    int2 Home = int2(floor(Library.Fold(Local) * CellSide));

    float PixelAngle = 2.0 / (View.ViewToClip[1][1] * View.ViewSizeAndInvSize.y);
    float Sigma = 0.72 * PixelAngle;
    float Low = 1.0 - saturate(Way.z);
    for (int Y = -1; Y <= 1; ++Y)
    {
        for (int X = -1; X <= 1; ++X)
        {
            int2 At = Home + int2(X, Y);
            if (At.x < 0 || At.y < 0 || At.x >= 640 || At.y >= 640)
            {
                continue;
            }
            for (int Slot = 0; Slot < 3; ++Slot)
            {
                float4 Place = StarCells.Load(int3(At.x * 6 + Slot * 2, At.y, 0));
                if (Place.b <= 0.0)
                {
                    break;
                }

                float Off = length(Library.Unfold((float2(At) + Place.rg) / CellSide) - Local);
                if (Off < Unit)
                {
                    float4 Look = StarCells.Load(int3(At.x * 6 + Slot * 2 + 1, At.y, 0));
                    float Phase = Look.a * 6.2831853;
                    float Rate = 5.0 + 6.0 * frac(Look.a * 17.0);
                    float Flicker = 1.0 + (0.1 + 0.5 * Low * Low * Low) * sin(SkyTime * Rate + Phase) * sin(SkyTime * Rate * 0.37 + Phase * 2.0);
                    float Flux = pow(2.0, (Place.b * 255.0 - 1.0) / 254.0 * 14.0);
                    float Peak = Flux * StarLight / (Sigma * Sigma) * Flicker * Night;
                    float Fade = 1.0 - Off * Off / (Unit * Unit); // to nothing where the search for stars ends
                    Space += Look.rgb * Peak * exp(-0.5 * Off * Off / (Sigma * Sigma)) * Fade * Fade;
                }
            }
        }
    }
}

// The coronas, under the planet, which hides them as it does the discs. 0.0173 and 0.0126 are the discs'
// radii, SunSize and SecondSunSize in Engine/Render/SceneTypes.h.
Space += Library.Corona(Way, SunWay, 0.0173, SunCorona, 0.0, StarPole) + Library.Corona(Way, SecondWay, 0.0126, SecondCorona, 2.7, StarPole);

float Block = 0.0; // how much of a sun's disc the planet and its rings hide along this ray
if (dot(Way, PlanetWay) > 0.85 && PlanetWay.z > -0.25)
{
    float3 Across = normalize(cross(PlanetAxis, PlanetWay));
    float3 Up = cross(PlanetWay, Across);
    float2 Spot = float2(dot(Way, Across), dot(Way, Up)) / 0.035; // PlanetSize in Engine/Render/SceneTypes.h
    float Reach = dot(Spot, Spot);
    if (Reach <= 25.0)
    {
        float Tilt = dot(PlanetAxis, PlanetWay);
        float RingDepth = -Spot.y * dot(PlanetAxis, Up) / (abs(Tilt) > 1e-3 ? Tilt : 1e-3);
        float3 RingPoint = Across * Spot.x + Up * Spot.y + PlanetWay * RingDepth;
        float Ring = Library.RingDensity(length(RingPoint)) * 0.6;
        float3 RingLook = float3(0.085, 0.12, 0.185) * (SunLight * Library.RingLight(RingPoint, SunWay) + SecondLight * Library.RingLight(RingPoint, SecondWay)) * 0.5;

        if (RingDepth > 0.0)
        {
            Space = lerp(Space, RingLook, Ring);
        }

        float Disc = 1.0 - smoothstep(0.86, 1.05, Reach);
        if (Disc > 0.0)
        {
            float3 Surface = Across * Spot.x + Up * Spot.y - PlanetWay * sqrt(max(1.0 - Reach, 0.0));
            float Latitude = dot(Surface, PlanetAxis);
            float Longitude = atan2(dot(Surface, Across), dot(Surface, cross(PlanetAxis, Across))) + PlanetSpin;
            float Girth = sqrt(max(1.0 - Latitude * Latitude, 0.0));
            float Flow = (sin(Longitude * 3.0 + Latitude * 7.0) * 0.06 + sin(Longitude * 5.0 - Latitude * 4.0) * 0.03) * Girth;
            float Bands = 0.5 + 0.35 * sin((Latitude + Flow) * 9.0) + 0.15 * sin((Latitude - Flow) * 21.0 + 0.8);
            float3 Fixed = float3(Girth * cos(Longitude), Girth * sin(Longitude), Latitude);
            float Side = 0.5 + 0.5 * dot(Fixed, float3(0.8, 0.6, 0.0));
            float Storm = smoothstep(0.84, 0.95, dot(Fixed, float3(0.60, 0.35, 0.72)) + Flow);
            float Basin = smoothstep(0.66, 0.9, dot(Fixed, float3(-0.72, -0.30, 0.62)) + Flow);
            float Lower = smoothstep(0.7, 0.92, dot(Fixed, float3(-0.2, -0.82, -0.53)) + Flow);
            float Tone = lerp(lerp(saturate(Bands) * 0.55 + Side * 0.45, 1.0, Storm), 0.0, max(Basin, Lower));
            float3 Albedo = lerp(float3(0.075, 0.102, 0.155), float3(0.112, 0.154, 0.228), Tone);
            float3 Lit = SunLight * saturate(dot(Surface, SunWay)) * Library.RingShade(Surface, SunWay, PlanetAxis)
                + SecondLight * saturate(dot(Surface, SecondWay)) * Library.RingShade(Surface, SecondWay, PlanetAxis);
            Space = lerp(Space, Albedo * Lit, Disc);
        }

        if (RingDepth <= 0.0)
        {
            Space = lerp(Space, RingLook, Ring);
        }
        Block = 1.0 - (1.0 - Disc) * (1.0 - Ring);
    }
}

// The air holds on to a little of the planet's light at night, more along the horizon and around the planet.
float Toward = saturate(dot(Way, PlanetWay));
float3 Glow = NightAir * (0.55 + 0.1 * Mass) * (1.0 + 1.5 * pow(Toward, 6.0));

// The camera is set for the ground, which would leave Unreal's air too dim and too green by day.
float3 Sky = Air * float3(1.5, 1.7, 2.3) + Glow;

// The blue-green sun in its own colour, as in the standalone game's Sky.hlsl: at the engine's brightness for
// a sun it burns out to white like the first, so it is only a few times the sky around it.
float InSecond = saturate(dot(SecondDisc, float3(1.0, 1.0, 1.0)) * 1e4);
float3 SecondShown = InSecond * float3(0.10, 1.0, 0.83) * (dot(Sky, float3(0.3, 0.6, 0.1)) * 4.0);
return Sky + Space * Through * Above + (FirstDisc + SecondShown) * (1.0 - Block);
'''


def make_sky():
    material = begin('M_Sky')
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('is_sky', True)
    material.set_editor_property('two_sided', True)

    scalars = ('Night', 'SkyTime', 'StarTurn', 'PlanetSpin')
    vectors = ('SunWay', 'SecondWay', 'PlanetWay', 'PlanetAxis', 'StarPole', 'SunLight', 'SecondLight', 'NightAir', 'SkyShift', 'SunCorona', 'SecondCorona')
    node = custom_node(material, SKY_CODE, ('Ray', 'Air', 'FirstDisc', 'SecondDisc') + scalars + vectors + ('StarCells',), -300, 0)

    ray = materials.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS, -700, -500)
    air = materials.create_material_expression(material, unreal.MaterialExpressionSkyAtmosphereViewLuminance, -700, -400)
    link(ray, '', node, 'Ray')
    link(air, '', node, 'Air')

    # As wide across as the standalone game draws them, in degrees.
    for index, (name, degrees) in enumerate((('FirstDisc', 1.98), ('SecondDisc', 1.44))):
        disc = materials.create_material_expression(material, unreal.MaterialExpressionSkyAtmosphereLightDiskLuminance, -700, -300 + index * 100)
        disc.set_editor_property('light_index', index)
        link(constant(material, degrees, -1000, -300 + index * 100), '', disc, '')
        link(disc, '', node, name)

    row = -100
    for name in scalars:
        link(scalar_parameter(material, name, 0.0, -700, row), '', node, name)
        row += 100
    for name in vectors:
        link(vector_parameter(material, name, -700, row), '', node, name)
        row += 100
    link(texture_parameter(material, 'StarCells', -700, row), '', node, 'StarCells')

    connect(node, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material)


def make_rain():
    material = begin('M_Rain')
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property('blendable_location', unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)

    # The standalone game's Shaders/Tonemap.hlsl: the rain falls only in front of whatever is further than it.
    names = ('Rain', 'Slant', 'RainTime', 'Scene', 'Depth')
    node = custom_node(material, RAIN_CODE, names, -300, 0)
    scene = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, -100)
    scene.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
    link(scene, 'Color', node, 'Scene')
    # Only here so the material asks for the depth: the code reads it by itself.
    depth = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, 0)
    depth.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_SCENE_DEPTH)
    link(depth, 'Color', node, 'Depth')
    link(scalar_parameter(material, 'Rain', 0.0, -700, 100), '', node, 'Rain')
    link(scalar_parameter(material, 'Slant', 0.0, -700, 200), '', node, 'Slant')
    link(scalar_parameter(material, 'RainTime', 0.0, -700, 300), '', node, 'RainTime')
    connect(node, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material)


def make_magic():
    material = begin('M_Magic')
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property('blendable_location', unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)

    scalars = ('MagicView', 'MagicBends', 'MagicTime')
    vectors = ('NearCorner', 'NearSize', 'FarCorner', 'FarSize', 'ViewForward', 'ViewRight', 'ViewUp') \
        + tuple('GlowPoint%d' % index for index in range(8)) + tuple('GlowColor%d' % index for index in range(8))
    textures = ('NearAtlas', 'FarAtlas')
    node = custom_node(material, MAGIC_CODE, scalars + vectors + textures + ('Scene', 'Depth'), -300, 0)

    row = -300
    for name in scalars:
        link(scalar_parameter(material, name, 0.0, -700, row), '', node, name)
        row += 100
    for name in vectors:
        # Where held magic is takes its radius along in the fourth number.
        link(vector_parameter(material, name, -700, row), 'RGBA' if name.startswith('GlowPoint') else '', node, name)
        row += 100
    for name in textures:
        link(texture_parameter(material, name, -700, row), '', node, name)
        row += 100

    # Only here so the material asks for these textures: the code reads them by itself.
    scene = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, row)
    scene.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
    depth = materials.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, row + 100)
    depth.set_editor_property('scene_texture_id', unreal.SceneTextureId.PPI_SCENE_DEPTH)
    link(scene, 'Color', node, 'Scene')
    link(depth, 'Color', node, 'Depth')

    connect(node, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material)


make_terrain()
make_flame()
make_puff()
make_bolt()
make_leaf()
make_body()
make_water()
make_underwater()
make_focus()
make_sky()
make_magic()
make_rain()
