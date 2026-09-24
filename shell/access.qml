//@ pragma AppId atrium-access

import QtQuick
import Atrium.Shell
import shell.modules.access

// The portal's "allow this?" question, run by atrium-portal, which hands it
// the question and reads back the answer.
ShellRoot {
    AccessDialog {}
}
