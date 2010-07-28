# Makefile substitutions required for partial linking to
# create a cexpsh-loadable object file
AC_DEFUN([TILLAC_RTEMS_OBJLINK],
	AC_SUBST([OBJLINK],[['$(CCLD) -nostdlib -Wl,-r -o $[@]']])
	AC_SUBST([OBJEXEEXT],['.obj'])
)
