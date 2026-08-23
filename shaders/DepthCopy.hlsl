Texture2D<float> DepthInput : register(t0);
RWTexture2D<float> DepthOutput : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	uint2 size;
	DepthInput.GetDimensions(size.x, size.y);
	if (dtid.x >= size.x || dtid.y >= size.y)
		return;
	DepthOutput[dtid.xy] = DepthInput[dtid.xy];
}
