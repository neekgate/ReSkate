// The Filmer's VX1000 look (filmer_look.cpp). Built into the DLL as text (cmake/UI.cmake) and compiled
// when the overlay first draws it; the build checks it with fxc when the Windows SDK has one.
cbuffer Look : register(b0)
{
    float4 L0; // bend 0..1, half angle the bend covers at the frame's top edge (rad), tan(half the game's FOV), frame aspect
    float4 L1; // source aspect, 4:3 bars, hood, seconds
    float4 L2; // grade, scanlines, grain, softness
    float4 L3; // transfer (0 SDR), exposure, output width, output height
    float4 L4; // hand centre x, y (half heights), hand angle (rad), hand on 0..1
    float4 L5; // hand size, mirrored, source width, source height
};
Texture2D source : register(t0);
SamplerState linear_clamp : register(s0);

struct Interpolated { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

Interpolated vs_main(uint id : SV_VertexID)
{
    Interpolated o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = p;
    return o;
}

float hash(float2 p)
{
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac((p.x + p.y) * p.x);
}

float capsule(float2 p, float2 a, float2 b, float r)
{
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h) - r;
}

// A hand right against the lens: palm, four fingers and a thumb, far too close to be in focus.
float hand_distance(float2 p)
{
    float palm = length((p - float2(0.0, -0.25)) / float2(0.58, 0.68)) - 1.0;
    palm *= 0.6;
    float d = palm;
    d = min(d, capsule(p, float2(-0.36, 0.25), float2(-0.42, 1.05), 0.12));
    d = min(d, capsule(p, float2(-0.12, 0.30), float2(-0.13, 1.25), 0.13));
    d = min(d, capsule(p, float2(0.12, 0.30), float2(0.14, 1.20), 0.13));
    d = min(d, capsule(p, float2(0.35, 0.22), float2(0.44, 0.95), 0.11));
    d = min(d, capsule(p, float2(-0.50, -0.35), float2(-1.00, 0.10), 0.14));
    return d;
}

float3 fetch(float2 uv)
{
    return source.SampleLevel(linear_clamp, uv, 0.0).rgb;
}

float4 ps_main(Interpolated i) : SV_Target
{
    float screen_aspect = L3.z / max(L3.w, 1.0);
    float2 c = i.uv * 2.0 - 1.0;
    float2 q = float2(c.x * screen_aspect, c.y);
    // 4:3: black at the sides.
    if (L1.y > 0.5 && abs(q.x) > L0.w)
        return float4(0.0, 0.0, 0.0, 1.0);
    // The bend: the MK1 squeezes a wide view into the frame. A point at radius r (half heights) sees
    // the direction r * the half angle off the axis; the game drew that direction at tan(angle) / tan(FOV / 2).
    float r = length(q);
    float2 along = r > 1e-5 ? q / r : float2(0.0, 0.0);
    float rs = r;
    if (L0.x > 0.0)
    {
        float theta = min(r * L0.y, 1.5);
        rs = lerp(r, tan(theta) / L0.z, L0.x);
    }
    float2 s = along * rs;
    float2 uv = float2(s.x / L1.x, s.y) * 0.5 + 0.5;
    float2 inside = min(uv, 1.0 - uv);
    float margin = min(inside.x, inside.y);
    if (margin < 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float2 texel = 1.0 / max(L5.zw, 1.0);
    // Standard definition: a few hundred lines, soft, with the camcorder's sharpening halo.
    float sd = max(L5.w / 480.0, 1.0);
    float3 colour;
    if (L2.w > 0.5)
    {
        float2 close_tap = texel * sd * 0.6;
        float2 wide = texel * sd * 1.8;
        float3 soft = (fetch(uv + float2(close_tap.x, 0)) + fetch(uv - float2(close_tap.x, 0)) + fetch(uv + float2(0, close_tap.y)) + fetch(uv - float2(0, close_tap.y))) * 0.25;
        float3 blur = (fetch(uv + float2(wide.x, wide.y)) + fetch(uv + float2(-wide.x, wide.y)) + fetch(uv + float2(wide.x, -wide.y)) + fetch(uv - wide)) * 0.25;
        colour = soft + (soft - blur) * 0.55;
        // Colour bleeds sideways on tape: the chroma comes from a little to the right.
        float3 right = fetch(uv + float2(texel.x * sd * 2.5, 0.0));
        float3 weights = float3(0.299, 0.587, 0.114);
        float luma = dot(colour, weights);
        float3 chroma = lerp(colour, right, 0.5) - dot(lerp(colour, right, 0.5), weights);
        colour = luma + chroma;
    }
    else
    {
        colour = fetch(uv);
    }
    bool sdr = L3.x < 0.5;
    if (sdr && L2.x > 0.5)
    {
        // The VX's colour: saturated, contrasty, warm, blacks a touch crushed.
        colour = saturate(colour);
        float luma = dot(colour, float3(0.299, 0.587, 0.114));
        colour = lerp(luma.xxx, colour, 1.2);
        colour = saturate(colour);
        float3 curve = colour * colour * (3.0 - 2.0 * colour);
        colour = lerp(colour, curve, 0.4);
        colour *= float3(1.05, 1.01, 0.93);
        colour = saturate((colour - 0.025) / 0.975);
    }
    if (sdr)
        colour *= L3.y; // the iris: down while the hand covers it, open too wide for a moment after
    // The hood: the fisheye's picture ends in a soft dark edge, and darkens towards it.
    if (L1.z > 0.5)
    {
        float edge = smoothstep(0.0, 0.035, margin);
        float falloff = 1.0 - 0.4 * smoothstep(0.75, 1.85, r * (L0.x > 0.0 ? 1.0 : 0.85));
        colour *= edge * falloff;
    }
    // Interlaced fields: every other line a little darker.
    if (L2.y > 0.5)
    {
        float row = floor(i.uv.y * min(L3.w, 480.0));
        colour *= (fmod(row, 2.0) < 1.0) ? 1.0 : 0.93;
    }
    if (sdr && L2.z > 0.5)
    {
        float n = hash(floor(i.uv * float2(720.0, 480.0)) + frac(L1.w * 37.0) * 91.0) - 0.5;
        colour += n * 0.045;
    }
    // The hand over the lens.
    if (L4.w > 0.0)
    {
        float2 p = q - L4.xy;
        p.x *= L5.y > 0.5 ? -1.0 : 1.0;
        float ca = cos(-L4.z), sa = sin(-L4.z);
        p = float2(p.x * ca - p.y * sa, p.x * sa + p.y * ca) / max(L5.x, 0.1);
        p.y = -p.y; // the picture's y runs down; the fingers point up
        float d = hand_distance(p);
        float cover = 1.0 - smoothstep(-0.12, 0.12, d);
        // Light comes through the skin at the edges and between the fingers: a red glow.
        float glow = exp(-abs(d) * 7.0) * (1.0 - cover * 0.6);
        float3 skin = float3(0.035, 0.022, 0.018) + float3(0.55, 0.12, 0.05) * glow * 0.6;
        if (!sdr)
            skin *= 0.5;
        colour = lerp(colour, skin, saturate(cover * L4.w + glow * 0.25 * L4.w));
    }
    return float4(colour, 1.0);
}
