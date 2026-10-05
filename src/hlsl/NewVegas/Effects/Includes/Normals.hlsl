float3 GetNormal( float2 uv)
{
	return tex2D (TESR_NormalsBuffer, uv).xyz * 2 - 1;
}


float3 GetWorldNormal( float2 uv)
{
	float4 normal = float4(GetNormal(uv), 1);
	return mul(TESR_ViewTransform, normal).xyz;
}

float3 GetWorldNormalLod(float2 uv)
{
	float4 normal = float4(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz * 2 - 1, 1);
	return mul(TESR_ViewTransform, normal).xyz;
}
