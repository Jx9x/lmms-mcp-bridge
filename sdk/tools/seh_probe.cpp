int risky(int x) {
    if (x < 0) throw 42;
    return x * 2;
}
int probe(int x) {
    try {
        return risky(x);
    } catch (int e) {
        return e;
    }
}
