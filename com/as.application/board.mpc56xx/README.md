

By default, this project support the machine MPC5634M_MLQB80 only, but event for this an empty template project should be created by codewarrior v2.10 IDE as below file structure.

```sh
MPC5634M_MLQB80
└── Project
    ├── Project_Data
    │   ├── RAM
    │   │   └── ObjectCode
    │   └── internal_FLASH
    ├── bin
    ├── lcf
    └── src
```

To support other kind of chips, please take the MPC5634M_MLQB80 as an example and modify the SConscript and Kconfig


https://www.nxp.com/docs/en/reference-manual/MPC563XMRM.pdf

The configuration file in directory "common" is for board TRK-MPC5634M REV B.
