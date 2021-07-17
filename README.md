# Debian packaging for Reverse Engineers' Hex Editor

This repository contains the packaging metadata used to generate packages for Ubuntu and Debian. Please only open issues or pull requests here for issues relating to the **packaging** (i.e. stuff under the debian/ directories).

## Branches

Each distro/release has a dedicated branch to allow for any necessary changes between them (e.g. different dependency names/versions).

Maintained releases:

- debian/bullseye (Debian 11)
- debian/buster (Debian 10)
- ubuntu/hirsute (Ubuntu 21.04)
- ubuntu/focal (Ubuntu 20.04)
- ubuntu/bionic (Ubuntu 18.04)

Obsolete releases:

- debian/stretch (Debian 9)
- ubuntu/groovy (Ubuntu 20.10)
- ubuntu/eoan (Ubuntu 19.10)
- ubuntu/xenial (Ubuntu 16.04)

## Building snapshots

TODO

## Release process

TODO

## Building releases

TODO

## Package versioning

Packages use the following version numbers (where x.y.z is the rehex version):

    x.y.z-0~debian9
           ~debian10
           ~ubuntu1810
           ~ubuntu1904
           ~...

The tilde version on the Debian revision is so that an official (if one ever exists) package for the same version will be considered newer than ours and be preferred. The numeric distribution version ensures the correct version will be installed during a distribution upgrade.

The Debian revision is ZERO rather than ONE because Ubuntu use versions like x.y.z-0ubuntu1 where they haven't forked a Debian package, it remains zero elsewhere for consistency.

In the event a release must be made just for a packaging change (i.e. what is normally a Debian revision bump), an extra version shall be added after the distribution suffix:

    x.y.z-0~debian9
    x.y.z-0~debian9+1
    x.y.z-0~debian9+2
    ...

Snapshots:

    x.y.z+gitSHA-0~debian9

Debian revision follows same logic as above. The (short) Git SHA is added to end of the last non-shapshot version number so the next non-shapshot release will be considered updates. These are currently not intended to make it into a Debian repository, so their relative ordering doesn't matter and we don't need to include a timestamp in the version too.
