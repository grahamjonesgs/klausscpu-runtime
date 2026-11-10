/* Dropbear options for the KlaussCPU board: the host keys live in RAM
 * (/etc/dropbear, generated at first connection with -R); root may log in
 * with a password (the board's /etc/shadow). */
#define DSS_PRIV_FILENAME "/etc/dropbear/dropbear_dss_host_key"
#define RSA_PRIV_FILENAME "/etc/dropbear/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME "/etc/dropbear/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME "/etc/dropbear/dropbear_ed25519_host_key"
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_X11FWD 0
#define DEFAULT_PATH "/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH "/usr/sbin:/usr/bin:/sbin:/bin"
#define SFTPSERVER_PATH "/usr/libexec/sftp-server"
