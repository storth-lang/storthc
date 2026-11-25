pub PI      :: 3.1415926535897932;
pub TAU     :: 6.2831853071795865;
pub E       :: 2.7182818284590452;
pub SQRT2   :: 1.4142135623730950;
pub DEG2RAD :: 0.0174532925199433;
pub RAD2DEG :: 57.2957795130823209;

pub fn abs(x: f64) -> f64 {
    if x < 0.0 then return -x;
    return x;
}

pub fn min(a: f64, b: f64) -> f64 {
    if a < b then return a;
    return b;
}

pub fn max(a: f64, b: f64) -> f64 {
    if a > b  then return a;
    return b;
}

pub fn clamp(val: f64, low: f64, high: f64) -> f64 {
    return max(low, min(val, high));
}


pub fn lerp(a: f64, b: f64, t: f64) -> f64 {
    return a + t * (b - a);
}

pub fn floor(x: f64) -> f64 {
    i := cast(i64)x;
    f := cast(f64)i;
    if x < f then return f - 1.0;
    return f;
}

pub fn ceil(x: f64) -> f64 {
    i := cast(i64)x;
    f := cast(f64)i;
    if (x > f) { return f + 1.0; }
    return f;
}

pub fn sqrt(x: f64) -> f64 {
    if x <= 0.0 then return 0.0;
    guess := x;
    for i: 0..16 {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

pub fn pow(base: f64, exp: i32) -> f64 {
    if exp == 0 then return 1.0;
    if exp < 0 then return 1.0 / pow(base, -exp);

    result : f64 = 1.0;
    b := base;
    e := exp;

    for i: 0..exp {
        result = result * base;
    }

    return result;
}

pub fn sin(x: f64) -> f64 {
    sum := x;
    term := x;
    x2 := x * x;

    for i: 1..6 {
        fi := cast(f64)i;
        denom := (2.0 * fi) *  (2.0 * fi + 1.0);
        term = -term * x2 / denom;
        sum = sum + term;
    }
    return sum;
}


pub fn cos(x: f64) -> f64 {
    sum : f64 = 1.0;
    term : f64 = 1.0;
    x2 := x * x;

    for i: 1..6 {
        fi := cast(f64)i;
        denom := (2.0 * fi - 1.0) *  (2.0 * fi);
        term = -term * x2 / denom;
        sum = sum + term;
    }
    return sum;
}


pub fn tan(x: f64) -> f64 {
    return sin(x) / cos(x);
}

pub fn sec(x: f64) -> f64 {
    return 1.0 / cos(x);
}

pub fn csc(x: f64) -> f64 {
    return 1.0 / sin(x);
}

pub fn cot(x: f64) -> f64 {
    return cos(x) / sin(x);
}

pub struct Vector2 {
    x: $T;
    y: $T;
}

pub impl Vector2 {
    fn add(self: *Self, v: $T) {
        self.x += v;
        self.y += v;
    }

    fn add(self: *Self, v: Self) {
        self.x += v.x;
        self.y += v.y;
    }

    fn sub(self: *Self, v: $T) {
        self.x -= v;
        self.y -= v;
    }

    fn sub(self: *Self, v: Self) {
        self.x -= v.x;
        self.y -= v.y;
    }

    fn mult(self: *Self, v: $T) {
        self.x *= v;
        self.y *= v;
    }

    fn mult(self: *Self, v: Self) {
        self.x *= v.x;
        self.y *= v.y;
    }


    fn div(self: *Self, v: $T) {
        self.x /= v;
        self.y /= v;
    }

    fn div(self: *Self, v: Self) {
        self.x /= v.x;
        self.y /= v.y;
    }
}
