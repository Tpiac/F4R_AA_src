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
    return CameraData.w / (-depth * CameraData.z + CameraData.x);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    if (any(dispatchID.xy >= RenderSize))
        return;

    float depth = DepthInput[dispatchID.xy];
    float2 motionVector = MotionVectorInput[dispatchID.xy];
    float centerDist = GetScreenDepth(depth);
    float farFactor = smoothstep(80.0, 250.0, centerDist);

    if (farFactor > 0.0)
    {
        float renderScale = (float)RenderSize.x / (float)ScreenSize.x;
        float invScale = 1.0 - renderScale;
        float depthThresholdInner = lerp(0.97, 0.985, saturate(invScale / 0.5));
        float depthThresholdOuter = lerp(0.985, 0.992, saturate(invScale / 0.5));
        float mvPenalty = 0.40 / max(renderScale, 0.5);

        float blendStrength;
        if (renderScale > 0.62)
            blendStrength = 1.0;
        else if (renderScale > 0.54)
            blendStrength = 0.82;
        else
            blendStrength = 0.72;

        float2 mvAccum = 0.0;
        float totalWeight = 0.0;

        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                if (x == 0 && y == 0)
                    continue;

                int2 samplePos = int2(dispatchID.xy) + int2(x, y);
                if (any(samplePos < 0) || any(samplePos >= int2(RenderSize)))
                    continue;

                float neighborDist = GetScreenDepth(DepthInput[samplePos]);
                if (neighborDist < centerDist * depthThresholdInner)
                {
                    float w = 1.0 / (neighborDist * neighborDist * 0.0005 + 1.0);
                    float2 neighborMV = MotionVectorInput[samplePos];
                    float mvDiff = length(neighborMV - motionVector);
                    w *= exp(-mvDiff * mvPenalty);
                    mvAccum += neighborMV * w;
                    totalWeight += w;
                }
            }
        }

        if (totalWeight == 0.0)
        {
            for (int y = -2; y <= 2; y++)
            {
                for (int x = -2; x <= 2; x++)
                {
                    if (abs(x) <= 1 && abs(y) <= 1)
                        continue;

                    int2 samplePos = int2(dispatchID.xy) + int2(x, y);
                    if (any(samplePos < 0) || any(samplePos >= int2(RenderSize)))
                        continue;

                    float neighborDist = GetScreenDepth(DepthInput[samplePos]);
                    if (neighborDist < centerDist * depthThresholdOuter)
                    {
                        float w = 1.0 / (neighborDist * neighborDist * 0.0004 + 1.0);
                        float2 neighborMV = MotionVectorInput[samplePos];
                        float mvDiff = length(neighborMV - motionVector);
                        w *= exp(-mvDiff * (mvPenalty * 1.5));
                        w *= 0.45;
                        mvAccum += neighborMV * w;
                        totalWeight += w;
                    }
                }
            }
        }

        if (totalWeight > 0.0)
        {
            float2 fixedMotionVector = mvAccum / totalWeight;
            motionVector = lerp(motionVector, fixedMotionVector, farFactor * blendStrength);
        }
    }

    MotionVectorOutput[dispatchID.xy] = motionVector;
}
