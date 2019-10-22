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
		'-Wno-unused-parameter',
		'-Wundef',
		'-Wvla',
	],
	language: 'c',
)

lib_ml = library('ml', ['mainloop.c'])
executable('mltest', 'test.c',
	link_with: lib_ml)
