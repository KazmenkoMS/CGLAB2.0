#include "NoiseGeneration.h"
inline float hash2D(XMFLOAT2 p)
{
	// dot product
	float h = p.x * 12.9898f + p.y * 78.233f;
	return frac(std::sin(h) * 43758.5453123f);
}

// Функция плавного интерполирования (quintic/smoothstep)
// f(t) = 6t^5 - 15t^4 + 10t^3. Обеспечивает C2-непрерывность.
inline XMFLOAT2 fade(XMFLOAT2 t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// 2D Value Noise (похож на Perlin Noise)
// Возвращает одно значение шума в диапазоне [0, 1]
inline float noise2D(XMFLOAT2 p)
{
    // 1. Находим координаты сетки (ячейки)
    XMFLOAT2 i(std::floor(p.x), std::floor(p.y));

    // 2. Находим дробную часть (позицию внутри ячейки)
    XMFLOAT2 f(frac(p.x), frac(p.y));

    // 3. Вычисляем 4 псевдослучайных значения для углов ячейки
    float a = hash2D(i);
    float b = hash2D(i + XMFLOAT2(1.0f, 0.0f));
    float c = hash2D(i + XMFLOAT2(0.0f, 1.0f));
    float d = hash2D(i + XMFLOAT2(1.0f, 1.0f));

    // 4. Применяем функцию сглаживания (fade) к дробной части
    XMFLOAT2 u = fade(f);

    // 5. Выполняем билинейную интерполяцию (lerp)
    return lerp(
        lerp(a, b, u.x), // Интерполяция по X
        lerp(c, d, u.x), // Интерполяция по X
        u.y              // Интерполяция по Y
    );
}
// Функция FBM (Fractal Brownian Motion)
// Объединяет несколько октав шума для получения детализированного результата.
float FBM_Noise(XMFLOAT2 p)
{
    // Параметры FBM
    const int OCTAVES = 5;
    const float LACUNARITY = 2.0f;
    const float PERSISTENCE = 0.5f;

    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < OCTAVES; i++)
    {
        total += noise2D(p * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= PERSISTENCE;
        frequency *= LACUNARITY;
    }

    // Нормализация результата к диапазону [0, 1]
    return total / maxValue;
}