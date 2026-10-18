"""Independent 80-digit spherical disk area; mpmath 1.3.0, no OrbitForge imports."""
import mpmath as m

m.mp.dps = 80
EARTH_RADIUS = m.mpf(6378137)
SUN_RADIUS = m.mpf(695700000)


def norm(vector):
    return m.sqrt(sum(x * x for x in vector))


def ratio(position, sun):
    to_sun = [sun[i] - position[i] for i in range(3)]
    to_earth = [-x for x in position]
    solar_distance, earth_distance = norm(to_sun), norm(to_earth)
    dot = sum(to_sun[i] * to_earth[i] for i in range(3))
    if dot <= 0 or earth_distance >= solar_distance:
        return m.mpf(1)
    a = m.asin(SUN_RADIUS / solar_distance)
    b = m.asin(EARTH_RADIUS / earth_distance)
    d = m.acos(dot / solar_distance / earth_distance)
    if d >= a + b:
        return m.mpf(1)
    if d <= abs(b - a):
        return 1 - min(a, b)**2 / a**2
    x = (d*d + a*a - b*b) / (2*d*a)
    y = (d*d + b*b - a*a) / (2*d*b)
    area = (a*a*m.acos(x) + b*b*m.acos(y)
            - m.sqrt((-d+a+b)*(d+a-b)*(d-a+b)*(d+a+b)) / 2)
    return 1 - area / (m.pi*a*a)


def augment(text):
    lines = ["# Final V field: independent mpmath 1.3.0, 80-digit disk-area fraction"]
    for line in text.splitlines():
        fields = line.split()
        if fields and fields[0] == "C":
            sun = list(map(m.mpf, fields[-3:]))
        if fields and fields[0] == "V":
            position = list(map(m.mpf, fields[4:7]))
            line += " " + m.nstr(ratio(position, sun), 25)
        lines.append(line)
    return "\n".join(lines) + "\n"
