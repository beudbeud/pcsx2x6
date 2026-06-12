#!/usr/bin/env bash
# make-acgame.sh — génère un .acgame PCSX2x6 (System 246/256) et l'arborescence
# de fichiers nécessaire pour démarrer un jeu arcade à partir d'une image ISO/CHD.
#
# Format produit (lu par VMManager.cpp / isArcadeManifest + ACATA::SetEnv) :
#
#   <out>/<Nom>.acgame                 manifeste INI
#   <out>/roms/<slug>/<image>          mediasrc (.iso/.chd/.img/.bin)
#   <out>/roms/<slug>/<elf>            ELF de boot
#   <out>/roms/<slug>/<dongle>         dongle memcard (slot 1, optionnel)
#   <out>/roms/<slug>/<card>           carte de save (slot 2, optionnel)
#
# Exemple :
#   ./make-acgame.sh -i "Soul Calibur 3.iso" -e boot.elf -d NM00031.bin \
#       -n "Soul Calibur 3" -g SC3 -p 246 -j fighting -o ~/roms/namco2x6

set -euo pipefail

usage() {
	cat <<'EOF'
Usage: make-acgame.sh -i IMAGE (-e ELF | -x CHEMIN) -n NOM [options]

Requis :
  -i, --image FILE        image média (.iso/.chd/.img/.bin)
  -e, --elf FILE          ELF de boot (fichier local, ex: boot.elf)
  -x, --extract-elf PATH  extraire l'ELF directement de l'image (chemin dans
                          l'ISO, ex: SLPM_123.45 ou MODULES/BOOT.ELF)
  -n, --name NOM          nom du jeu (titre affiché, nom du .acgame)

Options :
  -L, --list           lister le contenu de l'image (pour trouver l'ELF) et quitter
  -g, --gameid ID      gameid/serial (adapte l'input JVS) [défaut: dérivé du nom]
  -p, --platform P     246 | 256 | super256                [défaut: 246]
  -m, --media TYPE     CD | DVD | HDD                      [défaut: deviné de l'image]
  -d, --dongle FILE    dongle memcard -> slot 1 (mc0:)
  -c, --card FILE      carte de save  -> slot 2 (mc1:)
  -j, --jvsmode MODE   fighting | lightgun
  -a, --args ARGS      arguments passés au jeu (data.args)
  -o, --out DIR        répertoire de sortie                 [défaut: .]
  -S, --symlink        lier l'image au lieu de la copier (économise l'espace ;
                       à éviter pour media=HDD qui écrit dans l'image)
  -h, --help           cette aide

L'extraction (-x/-L) lit l'ISO9660 avec bsdtar, 7z ou isoinfo (au premier
disponible). Les images .chd sont d'abord dépaquetées avec chdman.
EOF
	exit "${1:-0}"
}

die() { echo "erreur: $*" >&2; exit 1; }

image= elf= name= gameid= platform=246 media= dongle= card= jvsmode= gameargs= out=. symlink=0
extract_elf= do_list=0

while [ $# -gt 0 ]; do
	case "$1" in
		-i|--image)       image=$2; shift 2 ;;
		-e|--elf)         elf=$2; shift 2 ;;
		-x|--extract-elf) extract_elf=$2; shift 2 ;;
		-L|--list)        do_list=1; shift ;;
		-n|--name)        name=$2; shift 2 ;;
		-g|--gameid)      gameid=$2; shift 2 ;;
		-p|--platform)    platform=$2; shift 2 ;;
		-m|--media)       media=$2; shift 2 ;;
		-d|--dongle)      dongle=$2; shift 2 ;;
		-c|--card)        card=$2; shift 2 ;;
		-j|--jvsmode)     jvsmode=$2; shift 2 ;;
		-a|--args)        gameargs=$2; shift 2 ;;
		-o|--out)         out=$2; shift 2 ;;
		-S|--symlink)     symlink=1; shift ;;
		-h|--help)        usage 0 ;;
		*) die "option inconnue: $1 (voir --help)" ;;
	esac
done

[ -n "$image" ] || usage 1
[ -f "$image" ] || die "image introuvable: $image"

# ---------------------------------------------------------------------------
# Accès au contenu ISO9660 de l'image (liste + extraction).
# Pour un .chd, dépaquette d'abord vers un ISO temporaire avec chdman.
# ---------------------------------------------------------------------------
tmpdir=
cleanup() { [ -n "$tmpdir" ] && rm -rf "$tmpdir"; }
trap cleanup EXIT

iso_source() { # imprime le chemin d'un fichier ISO lisible pour $image
	case "${image,,}" in
		*.chd)
			command -v chdman >/dev/null || die "chdman requis pour lire un .chd (paquet mame-tools)"
			tmpdir=$(mktemp -d)
			echo "dépaquetage du CHD (chdman)..." >&2
			chdman extractdvd -i "$image" -o "$tmpdir/image.iso" >/dev/null 2>&1 \
				|| chdman extractcd -i "$image" -o "$tmpdir/image.cue" -ob "$tmpdir/image.iso" >/dev/null 2>&1 \
				|| die "chdman n'a pas pu dépaqueter: $image"
			echo "$tmpdir/image.iso"
			;;
		*)	echo "$image" ;;
	esac
}

iso_list() { # liste les fichiers de l'ISO $1
	local iso=$1
	if command -v bsdtar >/dev/null; then
		bsdtar -tf "$iso"
	elif command -v 7z >/dev/null; then
		7z l -ba "$iso" | awk '{print $NF}'
	elif command -v isoinfo >/dev/null; then
		isoinfo -i "$iso" -R -f 2>/dev/null || isoinfo -i "$iso" -f
	else
		die "aucun outil d'extraction ISO trouvé (installer bsdtar, 7z ou isoinfo)"
	fi
}

iso_extract() { # extrait $2 (chemin dans l'ISO $1) vers $3
	local iso=$1 path=$2 dst=$3
	if command -v bsdtar >/dev/null; then
		bsdtar -xOf "$iso" "$path" > "$dst" 2>/dev/null && [ -s "$dst" ] && return 0
		# certains ISO listent les noms avec le suffixe de version ISO9660 ";1"
		bsdtar -xOf "$iso" "$path;1" > "$dst" 2>/dev/null && [ -s "$dst" ] && return 0
	fi
	if command -v 7z >/dev/null; then
		7z e -so "$iso" "$path" > "$dst" 2>/dev/null && [ -s "$dst" ] && return 0
	fi
	if command -v isoinfo >/dev/null; then
		isoinfo -i "$iso" -R -x "/$path" > "$dst" 2>/dev/null && [ -s "$dst" ] && return 0
		isoinfo -i "$iso" -x "/${path^^};1" > "$dst" 2>/dev/null && [ -s "$dst" ] && return 0
	fi
	rm -f "$dst"
	return 1
}

if [ "$do_list" = 1 ]; then
	iso_list "$(iso_source)"
	exit 0
fi

[ -n "$name" ] || usage 1
if [ -n "$extract_elf" ]; then
	[ -z "$elf" ] || die "-e et -x sont exclusifs"
else
	[ -n "$elf" ] || usage 1
	[ -f "$elf" ] || die "ELF introuvable: $elf"
fi
[ -z "$dongle" ] || [ -f "$dongle" ] || die "dongle introuvable: $dongle"
[ -z "$card" ]   || [ -f "$card" ]   || die "carte introuvable: $card"

case "$platform" in 246|256|super256) ;; *) die "platform invalide: $platform (246|256|super256)" ;; esac
case "$jvsmode" in ""|fighting|lightgun) ;; *) die "jvsmode invalide: $jvsmode (fighting|lightgun)" ;; esac

# media : deviné depuis l'image si non fourni.
# CHD -> le sectorsize du CHD fait autorité côté core, DVD est un défaut sûr.
# ISO  > 750 Mo -> DVD, sinon CD. .img/.bin plat -> HDD probable (image disque dur).
if [ -z "$media" ]; then
	case "${image,,}" in
		*.chd) media=DVD ;;
		*.iso)
			size=$(stat -c%s "$image")
			if [ "$size" -gt $((750 * 1024 * 1024)) ]; then media=DVD; else media=CD; fi ;;
		*.img|*.bin|*.raw) media=HDD ;;
		*) media=DVD ;;
	esac
	echo "media non précisé -> $media (deviné)"
fi
case "$media" in CD|DVD|HDD) ;; *) die "media invalide: $media (CD|DVD|HDD)" ;; esac

if [ "$media" = HDD ] && [ "$symlink" = 1 ]; then
	echo "attention: media=HDD écrit dans l'image — le lien symbolique pointera sur l'original" >&2
fi

# gameid par défaut : nom en majuscules sans espaces/ponctuation, tronqué à 8.
if [ -z "$gameid" ]; then
	gameid=$(echo "$name" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9' | cut -c1-8)
	echo "gameid non précisé -> $gameid (dérivé du nom)"
fi

# slug du sous-répertoire : nom en minuscules, alphanumérique.
slug=$(echo "$name" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '_' | sed 's/^_//;s/_$//')
gamedir="$out/roms/$slug"
manifest="$out/$name.acgame"

[ -e "$manifest" ] && die "existe déjà: $manifest"

mkdir -p "$gamedir"

install_file() { # src
	local src=$1 base
	base=$(basename "$src")
	if [ "$symlink" = 1 ]; then
		ln -sf "$(realpath "$src")" "$gamedir/$base"
	else
		[ -e "$gamedir/$base" ] && die "existe déjà: $gamedir/$base"
		cp "$src" "$gamedir/$base"
	fi
	echo "$base"
}

img_base=$(install_file "$image")
if [ -n "$extract_elf" ]; then
	elf_base=$(basename "$extract_elf" | sed 's/;1$//')
	[ -e "$gamedir/$elf_base" ] && die "existe déjà: $gamedir/$elf_base"
	echo "extraction de '$extract_elf' depuis l'image..."
	iso_extract "$(iso_source)" "$extract_elf" "$gamedir/$elf_base" \
		|| die "introuvable dans l'image: $extract_elf (utiliser -L pour lister)"
	# Garde-fou : les ELF des disques System 2x6 sont souvent chiffrés (le vrai
	# boot ELF vient du dongle). Vérifie le magic \x7fELF.
	if [ "$(head -c4 "$gamedir/$elf_base" | od -An -tx1 | tr -d ' ')" != "7f454c46" ]; then
		echo "attention: '$elf_base' n'a pas de magic ELF — probablement chiffré ;" >&2
		echo "           le boot ELF vient en général du dump du dongle, pas du disque" >&2
	fi
else
	elf_base=$(install_file "$elf")
fi
dongle_base=; [ -n "$dongle" ] && dongle_base=$(install_file "$dongle")
card_base=;   [ -n "$card" ]   && card_base=$(install_file "$card")

{
	echo "[game]"
	echo "name=$name"
	echo "gameid=$gameid"
	echo "platform=$platform"
	echo ""
	echo "[data]"
	echo "subdir=roms/$slug"
	echo "elf=$elf_base"
	echo "media=$media"
	echo "mediasrc=$img_base"
	[ -n "$dongle_base" ] && echo "dongle=$dongle_base"
	[ -n "$card_base" ]   && echo "card=$card_base"
	[ -n "$jvsmode" ]     && echo "jvsmode=$jvsmode"
	[ -n "$gameargs" ]    && echo "args=$gameargs"
} > "$manifest"

echo ""
echo "généré :"
echo "  $manifest"
sed 's/^/    | /' "$manifest"
echo "  $gamedir/"
ls -1 "$gamedir" | sed 's/^/    /'
echo ""
echo "à lancer avec : retroarch -L pcsx2_libretro.so \"$manifest\""
