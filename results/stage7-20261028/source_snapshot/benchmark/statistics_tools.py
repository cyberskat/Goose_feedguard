"""Nearest-rank percentiles. Instrumentation overhead is reported, not subtracted."""
def quantiles(samples):
    if not samples or any(type(value) is not int or value < 0 for value in samples):
        raise ValueError("Expected nonempty nonnegative integer nanosecond samples")
    ordered = sorted(samples)
    def rank(numerator):
        position = (len(ordered) * numerator + 999) // 1000
        return ordered[position - 1]
    return {"p50": rank(500), "p99": rank(990), "p999": rank(999)}
