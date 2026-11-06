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

SURFACE_COLOR = SURFACE_WEIGHTS + '''
if (Kind < 0.125)
{
    return Tint;
}
if (Kind < 0.375)
{
    float2 Round = float2((Flat.x + Flat.y) * 0.8, Height * 0.3) / 8.0;
    return Texture2DSample(BarkColor, BarkColorSampler, Round).rgb * Tint * 4.5;
}
float3 Stone = lerp(Texture2DSample(RockColor, RockColorSampler, Close).rgb, Texture2DSampleLevel(RockColor, RockColorSampler, Large, 5.0).rgb, 0.3);
if (Kind < 0.75)
{
    return Stone * Tint * 4.5;
}
float3 Result = Stone * Rock;
Result += lerp(Texture2DSample(GrassColor, GrassColorSampler, Close).rgb, Texture2DSampleLevel(GrassColor, GrassColorSampler, Large, 5.0).rgb, 0.3) * Grass;
Result += lerp(Texture2DSample(SandColor, SandColorSampler, Close).rgb, Texture2DSampleLevel(SandColor, SandColorSampler, Large, 5.0).rgb, 0.3) * Sand;
Result += lerp(Texture2DSample(SnowColor, SnowColorSampler, Close).rgb, Texture2DSampleLevel(SnowColor, SnowColorSampler, Large, 5.0).rgb, 0.3) * Snow;
Result *= lerp(float3(1.0, 1.0, 1.0), lerp(float3(1.16, 1.0, 0.8), float3(0.82, 1.0, 1.1), Tint.g), Grass);
return Result * (0.8 + 0.4 * Broad) * Tint.r * 2.0;
'''

SURFACE_NORMAL = SURFACE_WEIGHTS + '''
if (Kind < 0.375)
{
    return Up;
}
float2 Bend = Texture2DSample(RockNormal, RockNormalSampler, Close).xy * 2.0 - 1.0;
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

# The standalone game's ShadeCard in Shaders/Surfaces.hlsli. The cards' own coordinates come in UV0,
# and UV1 says whether a card is needles rather than leaves and how light it is.
LEAF_COLOR = '''
float4 Texel = Which.x > 0.5 ? Texture2DSample(NeedleColor, NeedleColorSampler, Place) : Texture2DSample(LeafColor, LeafColorSampler, Place);
float3 Tone = lerp(float3(0.72, 0.72, 0.74), float3(1.3, 1.12, 0.74), Which.y);
return lerp(Texel.rgb, dot(Texel.rgb, float3(0.3, 0.6, 0.1)).xxx, 0.22) * Tone * 2.2;
'''

# Sampled finer than the pixel asks for, or at a distance a card's holes average away and it turns solid.
LEAF_MASK = '''
return (Which.x > 0.5 ? Texture2DSampleBias(NeedleColor, NeedleColorSampler, Place, -2.0) : Texture2DSampleBias(LeafColor, LeafColorSampler, Place, -2.0)).a;
'''


def make_terrain():
    material = begin('M_Terrain')
    material.set_editor_property('tangent_space_normal', False)

    names = ('World', 'Normal', 'Tint', 'Kind') + SURFACE_TEXTURES
    color = custom_node(material, SURFACE_COLOR, names, -300, 0)
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

    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(normal, '', unreal.MaterialProperty.MP_NORMAL)
    connect(constant(material, 0.9, -300, 600), '', unreal.MaterialProperty.MP_ROUGHNESS)
    finish(material)


def make_leaf():
    material = begin('M_Leaf')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('two_sided', True)
    material.set_editor_property('opacity_mask_clip_value', 0.42)

    names = ('Place', 'Which', 'LeafColor', 'NeedleColor')
    color = custom_node(material, LEAF_COLOR, names, -300, 0)
    mask = custom_node(material, LEAF_MASK, names, -300, 300)
    mask.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)

    place = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 0)
    which = materials.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 100)
    which.set_editor_property('coordinate_index', 1)
    leaf = texture_parameter(material, 'LeafColor', -800, 200)
    needle = texture_parameter(material, 'NeedleColor', -800, 320)
    for node in (color, mask):
        link(place, '', node, 'Place')
        link(which, '', node, 'Which')
        link(leaf, '', node, 'LeafColor')
        link(needle, '', node, 'NeedleColor')

    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(mask, '', unreal.MaterialProperty.MP_OPACITY_MASK)
    connect(constant(material, 0.75, -300, 500), '', unreal.MaterialProperty.MP_ROUGHNESS)
    finish(material)


def make_body():
    material = begin('M_Body')
    material.set_editor_property('used_with_instanced_static_meshes', True)
    tint = materials.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -400, 0)
    tint.set_editor_property('parameter_name', 'Tint')
    tint.set_editor_property('default_value', unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    connect(tint, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(constant(material, 0.7, -400, 200), '', unreal.MaterialProperty.MP_ROUGHNESS)
    finish(material)


def make_water():
    material = begin('M_Water')
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property('translucency_lighting_mode', unreal.TranslucencyLightingMode.TLM_SURFACE)
    material.set_editor_property('two_sided', True) # it is seen from under the water as well

    color = materials.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 0)
    color.set_editor_property('constant', unreal.LinearColor(0.012, 0.075, 0.11, 1.0))
    connect(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    connect(constant(material, 0.08, -400, 150), '', unreal.MaterialProperty.MP_ROUGHNESS)
    connect(constant(material, 0.82, -400, 300), '', unreal.MaterialProperty.MP_OPACITY)
    finish(material)


# The standalone game's eye focus: sharp around the point of gaze, blurred away from it.
FOCUS_CODE = '''
float2 ViewUv = GetViewportUV(Parameters);
float Aspect = View.ViewSizeAndInvSize.x * View.ViewSizeAndInvSize.w;
float Distance = length((ViewUv - Center.xy) * float2(Aspect, 1.0));
float Amount = smoothstep(Radius, Radius * 2.6, Distance) * Strength;
float3 Sharp = SceneTextureLookup(ViewportUVToSceneTextureUV(ViewUv, 14), 14, false).rgb;
if (Amount <= 0.0)
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
    float2 Offset = float2(cos(Angle) / Aspect, sin(Angle)) * sqrt(Share) * 0.016;
    Sum += SceneTextureLookup(ViewportUVToSceneTextureUV(saturate(ViewUv + Offset), 14), 14, true).rgb * Weight;
    Total += Weight;
}
float Dimmed = 1.0 - 0.05 * smoothstep(Radius * 2.2, Radius * 3.4, Distance);
return lerp(Sharp, Sum / Total, Amount) * Dimmed;
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
        float Density = Texel.r * Texel.r * 24.0;
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
float Limit = min(SceneDistance, 3500.0);

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
        float Taken = 1.0 - exp(-Density * 0.0035 * Span);
        Glow += Sampled.yzw / Density * Clear * Taken;
        Clear *= 1.0 - Taken;
        Dense += saturate((Density - 5.5) / 2.5) * Span;
    }
    Near = Far;
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
float Grey = dot(Linear, float3(0.2126, 0.7152, 0.0722));
float3 Dimmed = lerp(Linear, float3(Grey, Grey, Grey), 0.45) * 0.22;
Linear = lerp(Linear, Dimmed * Clear + Glow * 0.9, MagicView);
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
    float3 Local = Way * Cosine - cross(StarPole, Way) * Sine + StarPole * dot(StarPole, Way) * (1.0 - Cosine);
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

float Block = 0.0; // how much of a sun's disc the planet and its rings hide along this ray
if (dot(Way, PlanetWay) > 0.85 && PlanetWay.z > -0.25)
{
    float3 Across = normalize(cross(PlanetAxis, PlanetWay));
    float3 Up = cross(PlanetWay, Across);
    float2 Spot = float2(dot(Way, Across), dot(Way, Up)) / 0.06;
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

return Air + Space * Through * Above + (FirstDisc + SecondDisc) * (1.0 - Block);
'''


def make_sky():
    material = begin('M_Sky')
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('is_sky', True)
    material.set_editor_property('two_sided', True)

    scalars = ('Night', 'SkyTime', 'StarTurn', 'PlanetSpin')
    vectors = ('SunWay', 'SecondWay', 'PlanetWay', 'PlanetAxis', 'StarPole', 'SunLight', 'SecondLight')
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


def make_magic():
    material = begin('M_Magic')
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property('blendable_location', unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)

    scalars = ('MagicView', 'MagicBends', 'MagicTime')
    vectors = ('NearCorner', 'NearSize', 'FarCorner', 'FarSize', 'ViewForward', 'ViewRight', 'ViewUp')
    textures = ('NearAtlas', 'FarAtlas')
    node = custom_node(material, MAGIC_CODE, scalars + vectors + textures + ('Scene', 'Depth'), -300, 0)

    row = -300
    for name in scalars:
        link(scalar_parameter(material, name, 0.0, -700, row), '', node, name)
        row += 100
    for name in vectors:
        link(vector_parameter(material, name, -700, row), '', node, name)
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
make_leaf()
make_body()
make_water()
make_focus()
make_sky()
make_magic()
