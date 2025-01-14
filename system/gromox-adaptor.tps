[Unit]
Description=Gromox adaptor service
Documentation=man:adaptor(8gx)

[Service]
Type=simple
User=gromox
ExecStart=@libexecdir@/gromox/adaptor
ExecReload=/bin/kill -HUP $MAINPID
ProtectSystem=yes

[Install]
WantedBy=multi-user.target
