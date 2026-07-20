// math.c - Minimal math functions for stb_truetype
// These are used by the TrueType font renderer.

double floor(double x) {
    long long i = (long long)x;
    return (x < 0 && x != (double)i) ? (double)(i - 1) : (double)i;
}

double ceil(double x) {
    long long i = (long long)x;
    return (x > 0 && x != (double)i) ? (double)(i + 1) : (double)i;
}

double fabs(double x) {
    return x < 0 ? -x : x;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - (long long)(x / y) * y;
}

double sqrt(double x) {
    if (x <= 0) return 0;
    double guess = x / 2.0;
    for (int i = 0; i < 30; i++) {
        guess = (guess + x / guess) / 2.0;
    }
    return guess;
}

double pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (exp == 1.0) return base;
    if (base == 0.0) return 0.0;

    // Integer exponent fast path
    if (exp == (double)(long long)exp && exp > 0) {
        double result = 1.0;
        long long e = (long long)exp;
        double b = base;
        while (e > 0) {
            if (e & 1) result *= b;
            b *= b;
            e >>= 1;
        }
        return result;
    }

    // General case: exp(exp * ln(base))
    if (base < 0) return 0.0;

    // ln(x) via series: ln(x) = 2 * sum( ((x-1)/(x+1))^(2k+1) / (2k+1) )
    double ln_base = 0.0;
    {
        double u = (base - 1.0) / (base + 1.0);
        double u2 = u * u;
        double term = u;
        for (int k = 0; k < 30; k++) {
            ln_base += term / (2 * k + 1);
            term *= u2;
        }
        ln_base *= 2.0;
    }

    // exp(y) via Taylor series
    double val = exp * ln_base;
    double result = 1.0;
    double term = 1.0;
    for (int k = 1; k < 30; k++) {
        term *= val / k;
        result += term;
    }
    return result;
}

// cos(x) via Taylor series
double cos(double x) {
    double pi = 3.14159265358979323846;
    double two_pi = 2.0 * pi;
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;

    double x2 = x * x;
    double result = 1.0;
    double term = 1.0;
    for (int k = 1; k < 15; k++) {
        term *= -x2 / ((2*k - 1) * (2*k));
        result += term;
    }
    return result;
}

// acos(x) via Newton's method on cos
double acos(double x) {
    if (x >= 1.0) return 0.0;
    if (x <= -1.0) return 3.14159265358979323846;
    double pi = 3.14159265358979323846;
    double guess = pi / 2.0 - x;
    for (int i = 0; i < 20; i++) {
        double ct = cos(guess);
        double st = sqrt(1.0 - ct * ct);
        if (st < 1e-15) break;
        guess -= (ct - x) / (-st);
    }
    return guess;
}
