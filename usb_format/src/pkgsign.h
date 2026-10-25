#ifndef PKGSIGN_H
#define PKGSIGN_H

/* PS4-Paket (7F 43 4E 54): fPKG (Fake-Paket) oder offizielles PKG? Der Paketkopf unterscheidet beide nicht - aber der Image-
 * Schlüssel eines fPKG ist mit dem öffentlich bekannten Fake-Schlüssel verschlüsselt, den jeder entschlüsseln kann, der
 * eines offiziellen mit Sonys Schlüssel (nicht entschlüsselbar). Liest nur wenige KB am Anfang der Datei.
 * Liefert 1 = fPKG, 0 = offizielles PKG, -1 = nicht feststellbar (Datei nicht lesbar, kein PS4-Kopf, Schlüssel fehlen). */
int pkg4_is_fake(int fd);

#endif
