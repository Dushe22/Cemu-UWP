struct GSIn { float4 pos : SV_Position; };
struct GSOut { float4 pos : SV_Position; };
float4 expand(float4 p, float2 o) { return p + float4(o, 0, 0); }
float4 neverUsed(float4 p) { return p * 4; }
[maxvertexcount(4)]
void main(point GSIn input[1], inout TriangleStream<GSOut> stream)
{
	GSOut o;
	o.pos = expand(input[0].pos, float2(-1, -1)); stream.Append(o);
	o.pos = expand(input[0].pos, float2(1, -1)); stream.Append(o);
	o.pos = expand(input[0].pos, float2(-1, 1)); stream.Append(o);
	o.pos = expand(input[0].pos, float2(1, 1)); stream.Append(o);
}
