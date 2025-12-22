#ifdef MAIN_MODULE

int get_prime_one(void);
int get_prime_two(void);

int main(void) {
    return get_prime_one() + get_prime_two();
}

#elif defined(MODULE_ONE)

int get_prime_one(void) {
    return 17;
}

#elif defined(MODULE_TWO)

int get_prime_two(void) {
    return 23;
}

#endif
