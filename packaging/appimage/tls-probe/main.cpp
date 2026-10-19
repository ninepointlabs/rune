// A check for smoke.sh, not part of the AppImage: run from inside the
// extracted AppDir, it loads Qt's TLS backend the way Rune's AI requests do
// and reports which OpenSSL it found and how many of the host's CA
// certificates it can see. Exits 1 if TLS would not work.

#include <QCoreApplication>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const bool supported = QSslSocket::supportsSsl();
    const auto cas = QSslConfiguration::systemCaCertificates();
    out << "backend " << QSslSocket::activeBackend() << ", " << QSslSocket::sslLibraryVersionString()
        << ", " << cas.size() << " system CA certificates\n";
    return supported && !cas.isEmpty() ? 0 : 1;
}
