#include "../../tests/synthetic_profile.h"
#include "profilestore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringConverter>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct Fixture {
    int argc = 1;
    char name[36] = "omawin365-synthetic-profile-fuzzer";
    char* argv[2] = {name, nullptr};
    QCoreApplication application{argc, argv};
    QString source = QDir(QString::fromUtf8(qgetenv("OMAWIN365_FUZZ_FIXTURE")))
                         .filePath(QStringLiteral("synthetic.rdpw"));
    ProfileStore store;
};
Fixture* fixture = nullptr;
const QByteArray requiredSettings = supportedProfile;

void require(bool condition)
{
    if (!condition)
        std::abort();
}
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    // Runner supplies a fresh private fixture root. Never consult real XDG/HOME.
    const QByteArray root = qgetenv("OMAWIN365_FUZZ_FIXTURE");
    struct stat st {};
    require(!root.isEmpty() && root.startsWith('/')
            && ::lstat(root.constData(), &st) == 0 && S_ISDIR(st.st_mode)
            && st.st_uid == ::getuid() && (st.st_mode & 0777) == 0700);
    qputenv("HOME", root);
    qputenv("XDG_DATA_HOME", root + "/data");
    qputenv("XDG_CONFIG_HOME", root + "/config");
    qputenv("XDG_CACHE_HOME", root + "/cache");
    // Register destruction only AFTER QCoreApplication's constructor has
    // initialized Qt globals. Namespace-static owning pointers would be torn
    // down after those globals, causing a harness-only exit crash in Qt.
    static Fixture owned;
    fixture = &owned;
    require(fixture->store.profiles().isEmpty());
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // One mode byte plus up to 1 MiB + 1: exercise importFile's size guard too.
    constexpr size_t maximumInput = 1024 * 1024 + 2;
    if (!size || size > maximumInput)
        return 0;
    auto* store = &fixture->store;
    const auto& source = fixture->source;
    QByteArray bytes(reinterpret_cast<const char*>(data + 1), qsizetype(size - 1));
    switch (data[0] % 4) {
    case 1:
        bytes.prepend(requiredSettings);
        break;
    case 2:
    case 3: {
        // Raw mode preserves malformed encoding; these modes drive valid UTF-16
        // decoding and syntax mutations after the fixed required fields.
        const auto encoding = data[0] % 4 == 2 ? QStringConverter::Utf16LE : QStringConverter::Utf16BE;
        QStringEncoder encoder(encoding, QStringConverter::Flag::WriteBom);
        bytes = encoder.encode(QString::fromUtf8(requiredSettings + bytes));
        break;
    }
    default:
        break;
    }
    require(bytes.size() <= 2 * qsizetype(maximumInput + requiredSettings.size()));
    {
        QFile input(source);
        require(input.open(QIODevice::WriteOnly | QIODevice::Truncate));
        require(input.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        require(input.write(bytes) == bytes.size());
    }
    QString error;
    const Profile profile = store->importFile(source, &error);
    if (profile.id.isEmpty()) {
        require(!error.isEmpty() && store->profiles().isEmpty());
    } else {
        require(error.isEmpty() && store->profiles().size() == 1 && profile.path != source);
        QFile copy(profile.path);
        require(copy.open(QIODevice::ReadOnly));
        require(copy.readAll() == bytes);
        copy.close();
        // Same public fixture-owned lifecycle as tst_profiles; no corpus/crash
        // artifact is deleted. Keep persistent fixture size bounded.
        require(store->removeProfile(profile.id, &error));
        require(store->profiles().isEmpty());
    }
    QFile input(source);
    require(input.open(QIODevice::ReadOnly) && input.readAll() == bytes);
    return 0;
}
