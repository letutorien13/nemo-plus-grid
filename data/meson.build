
################################################################################
dataConf = configuration_data()
dataConf.set('VERSION', meson.project_version())

if get_option('system_integration')
  desktopFiles = [
    configure_file(
      input : 'nemo-autorun-software.desktop.in',
      output: 'nemo-autorun-software.desktop',
      configuration: dataConf,
    ),

    configure_file(
      input : 'nemo-autostart.desktop.in',
      output: 'nemo-autostart.desktop',
      configuration: dataConf,
    ),

    configure_file(
      input : 'nemo.desktop.in',
      output: 'nemo.desktop',
      configuration: dataConf,
    )
  ]

  install_data(desktopFiles,
    install_dir: join_paths(get_option('datadir'), 'applications')
  )
else
  # Isolated copy: one menu entry. Do not install nemo.desktop,
  # autostart, or autorun — those compete with system Nemo.
  install_data('nemo-plus.desktop.in',
    rename: 'nemo-plus.desktop',
    install_dir: join_paths(get_option('datadir'), 'applications')
  )
endif

################################################################################
dataConf = configuration_data()
dataConf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

if get_option('system_integration')
  dbusFiles = [
    configure_file(
      input : 'nemo.service.in',
      output: 'nemo.service',
      configuration: dataConf,
    ),

    configure_file(
      input : 'nemo.FileManager1.service.in',
      output: 'nemo.FileManager1.service',
      configuration: dataConf,
    ),
  ]

  install_data(dbusFiles,
    install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services')
  )
endif

################################################################################

mimeFile_translated = custom_target('nemo.xml',
  input : ['nemo.xml.in'],
  output: 'nemo.xml',
  command: ['intltool-merge',
    '--quiet',
    '--xml-style',
    '--utf8',
    po_subdir, '@INPUT@', '@OUTPUT@'
  ],
  install: true,
  install_dir: join_paths(get_option('datadir'), 'mime', 'packages'),
  build_by_default: true,
)

################################################################################
dataConf = configuration_data()
dataConf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

if get_option('system_integration')
  rootPolicyFile = configure_file(
    input : 'org.nemo.root.policy.in',
    output: 'org.nemo.root.policy',
    configuration: dataConf,
  )

  install_data(rootPolicyFile,
    install_dir: join_paths(get_option('datadir'), 'polkit-1', 'actions')
  )
endif

foreach lang_file: ['nemo_action.lang', 'nemo_search_helper.lang']
  foreach sv_dir: ['gtksourceview-2.0', 'gtksourceview-3.0', 'gtksourceview-4']
    install_data(lang_file,
      install_dir: join_paths(get_option('datadir'), sv_dir, 'language-specs')
    )
  endforeach
endforeach

################################################################################

subdir('icons')
