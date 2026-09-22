# Third-party source availability

Official Xenon Recomp binary releases redistribute dynamically linked components
that are available under the GNU LGPL, including Qt and the Xenia-maintained
FFmpeg build used for Xbox XMA frame decoding.

For every official binary release, the same GitHub release must include a
`Xenon-Third-Party-Sources-<version>.zip` asset containing the corresponding
source archives for the LGPL components shipped by that release, plus a machine
readable manifest of their versions and hashes.

The release automation treats this source bundle as a required release artifact.
A binary release should not be published if generation of the matching source
bundle fails.

The source bundle is provided to make the redistributed library sources
available alongside the binaries. Xenon's own source remains available under
its MIT license in the project repository. See `THIRD_PARTY_NOTICES.md` for the
licenses and attribution of each dependency.
