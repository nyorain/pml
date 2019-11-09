project(
	'ml',
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


lib_ml = library('posix_mainloop', ['mainloop.c'],
	install: true)

posix_mainloop_dep = declare_dependency(
	include_directories: [include_directories('.')],
	link_with: lib_ml)

if get_option('examples')
	dep_pulse = dependency('libpulse')
	dep_playerctl = dependency('playerctl')
	executable('mlexample',
		'example.c',
		dependencies: [dep_pulse, dep_playerctl, posix_mainloop_dep])
endif

if get_option('tests')
	mltest_nested = executable('mltest-nested',
		'test-nested.c',
		dependencies: [posix_mainloop_dep])
	test('nested', mltest_nested)

	mltest_nested_destroy = executable('mltest-nested-destroy',
		'test-nested-destroy.c',
		dependencies: [posix_mainloop_dep])
	test('nested-destroy', mltest_nested_destroy)
endif
