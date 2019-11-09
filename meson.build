project(
	'pml',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.48.0',
	default_options: [
		'c_std=c11',
		'warning_level=3',
		'werror=true',
	],
)

add_project_arguments(
	[
		'-Wundef',
		'-Wno-unused-parameter',
		'-Wno-unused-function',
		'-Wno-unused-const-variable',
	],
	language: 'c',
)

pml_inc = include_directories('.')
pml_lib = library('pml', ['pml.c'], install: true)
pml_dep = declare_dependency(
	include_directories: pml_inc,
	version: meson.project_version(),
	link_with: pml_lib)

install_headers('pml.h')

pkg = import('pkgconfig')
pkg.generate(
	pml_lib,
	name: 'pml',
	version: meson.project_version(),
	url: 'https://github.com/nyorain/pml',
	description: 'Simple POSIX mainloop for C')

# other stuff, examples & tests
if get_option('examples')
	dep_pulse = dependency('libpulse')
	dep_playerctl = dependency('playerctl')
	executable('pmlexample',
		'example.c',
		dependencies: [dep_pulse, dep_playerctl, pml_dep])
endif

if get_option('tests')
	mltest_nested = executable('test-nested',
		'test-nested.c',
		dependencies: [pml_dep])
	test('nested', mltest_nested)

	mltest_nested_destroy = executable('test-nested-destroy',
		'test-nested-destroy.c',
		dependencies: [pml_dep])
	test('nested-destroy', mltest_nested_destroy)
endif
