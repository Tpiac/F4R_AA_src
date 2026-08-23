// Community Shaders (EncodeTexturesCS.hlsl)
// Upscaling mod (DilateMotionVectorCS.hlsl), both GPL-3.0-or-later.
// Modified for use in this project.

cbuffer MotionVectorConstants : register(b0)
{
	uint2 ScreenSize;
	uint2 RenderSize;
	float4 CameraData;
};

Texture2D<float2> MotionVectorInput : register(t0);
Texture2D<float> DepthInput : register(t1);

RWTexture2D<float2> MotionVectorOutput : register(u0);

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= RenderSize))
		return;

	float depth = DepthInput[dispatchID.xy];
	float2 motionVector = MotionVectorInput[dispatchID.xy];
	float centerDist = GetScreenDepth(depth);

	if (centerDist > 100.0) {
		float2 mvAccum = 0.0;
		float totalWeight = 0.0;

		[unroll] for (int y = -2; y <= 2; y++)
		{
			[unroll] for (int x = -2; x <= 2; x++)
			{
				int2 samplePos = int2(dispatchID.xy) + int2(x, y);

				if (any(samplePos < 0) || any(samplePos >= int2(RenderSize)))
					continue;

				float neighborDist = GetScreenDepth(DepthInput[samplePos]);

				if (neighborDist < centerDist * 0.97) {
					float w = 1.0 / (neighborDist * neighborDist + 1.0);
					mvAccum += MotionVectorInput[samplePos] * w;
					totalWeight += w;
				}
			}
		}

		if (totalWeight > 0.0)
			motionVector = mvAccum / totalWeight;
	}

	MotionVectorOutput[dispatchID.xy] = motionVector;
}
