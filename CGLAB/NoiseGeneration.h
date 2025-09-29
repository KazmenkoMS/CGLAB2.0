#pragma once
#include "d3dUtil.h"
using namespace DirectX;
using namespace DirectX::PackedVector;

// operations for vectors (need for noise generation)
// Умножение на скаляр
inline XMFLOAT2 operator*(const XMFLOAT2& v, float s)
{
	return XMFLOAT2(v.x * s, v.y * s);
}

inline XMFLOAT2 operator*(float s, const XMFLOAT2& v)
{
	return XMFLOAT2(v.x * s, v.y * s);
}

// Поэлементное умножение векторов
inline XMFLOAT2 operator*(const XMFLOAT2& v1, const XMFLOAT2& v2)
{
	return XMFLOAT2(v1.x * v2.x, v1.y * v2.y);
}

// Сложение
inline XMFLOAT2 operator+(const XMFLOAT2& v1, const XMFLOAT2& v2)
{
	return XMFLOAT2(v1.x + v2.x, v1.y + v2.y);
}

inline XMFLOAT2 operator+(const XMFLOAT2& v, float s)
{
	return XMFLOAT2(v.x + s, v.y + s);
}

inline XMFLOAT2 operator+(float s, const XMFLOAT2& v)
{
	return XMFLOAT2(v.x + s, v.y + s);
}

// Вычитание
inline XMFLOAT2 operator-(const XMFLOAT2& v1, const XMFLOAT2& v2)
{
	return XMFLOAT2(v1.x - v2.x, v1.y - v2.y);
}

inline XMFLOAT2 operator-(const XMFLOAT2& v, float s)
{
	return XMFLOAT2(v.x - s, v.y - s);
}

inline XMFLOAT2 operator-(float s, const XMFLOAT2& v)
{
	return XMFLOAT2(s - v.x, s - v.y);
}

// Унарный минус
inline XMFLOAT2 operator-(const XMFLOAT2& v)
{
	return XMFLOAT2(-v.x, -v.y);
}

inline float lerp(float a, float b, float t)
{
	return a + t * (b - a);
}

inline XMFLOAT2 vecmul(XMFLOAT2& v1, XMFLOAT2& v2)
{
	return XMFLOAT2(v1.x * v2.x, v1.y * v2.y);
}
inline XMFLOAT2 vecmul(XMFLOAT2& v1, float f)
{
	return XMFLOAT2(v1.x * f, v1.y * f);
}
inline float frac(float x)
{
	return x - std::floor(x);
}
inline XMFLOAT2 frac(XMFLOAT2 v)
{
	return v - XMFLOAT2(frac(v.x), frac(v.y));
}
inline float hash2D(XMFLOAT2 p);

// Функция плавного интерполирования (quintic/smoothstep)
// f(t) = 6t^5 - 15t^4 + 10t^3. Обеспечивает C2-непрерывность.
inline XMFLOAT2 fade(XMFLOAT2 t);

// 2D Value Noise (похож на Perlin Noise)
// Возвращает одно значение шума в диапазоне [0, 1]
inline float noise2D(XMFLOAT2 p);

// Функция FBM (Fractal Brownian Motion)
// Объединяет несколько октав шума для получения детализированного результата.
 float FBM_Noise(XMFLOAT2 p);