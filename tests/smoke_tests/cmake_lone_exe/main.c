#include <stdio.h>

void lift_const(int argc, char** argv) {
	// jump past initializer to trigger variable rebinding in translated code
	if (argc == 3) { goto three_args; }

	// XREF:relooper_lifted_decls_must_be_mutable
	const int starts_with_a = *argv[0] == 'a';
	if (starts_with_a) {
		printf("  (arg 0 starts with 'a')\n");
	}

	three_args:
	if (argc == 3) {
		printf("  (argc was 3)\n");
	}
}

int main(int argc, char** argv)
{
  printf("Hello, Tenjin!\n");
  lift_const(argc, argv);
  return 0;
}