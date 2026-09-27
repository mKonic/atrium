// SMPTE ST 2084 (PQ). It raises to the 78.8th power: half precision is
// visibly wrong, so everything here is highp.

vec3 pq_decode(vec3 e) {
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
	return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}

vec3 pq_encode(vec3 l) {
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 p = pow(clamp(l, 0.0, 1.0), vec3(m1));
	return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}
