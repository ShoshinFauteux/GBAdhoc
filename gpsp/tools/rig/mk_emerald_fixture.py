"""Derive emerald_battle_{host,join}.inputs from the FR/LG battle fixtures by
symbol substitution (pret/pokeemerald symbols branch, pokeemerald.sym), then
replace the boot/walk section, which differs structurally."""
R = 'C:/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/wireless-worktree/testdata/fixtures/'
SUB = [
    ("0x030030F4", "0x030022C4"),   # gMain.callback2
    ("0x080565C9", "0x08085E5D"),   # CB2_Overworld|1
    ("0x08011115", "0x08038421"),   # BattleMainCB2|1
    ("0x0203ADE8", "0x0203CD94"),   # sMenu.maxCursorPos (menu.c sMenu 0x0203CD90)
    ("0x0203ADE6", "0x0203CD92"),   # sMenu.cursorPos
    ("0x0202004F", "0x020201CB"),   # sTextPrinters[0].active (0x020201B0+0x1B)
    ("0x03000EB1", "0x03000E41"),   # sGlobalScriptContext.mode
    ("0x03000FA4", "0x0203761C"),   # sSaveDialogCallback (EWRAM in Emerald)
    ("0x0806F80D", "0x080A0109"),   # SaveConfirmInputCallback|1
    ("0x0806F8F1", "0x080A01ED"),   # SaveOverwriteInputCallback|1
    ("0x0806F9F5", "0x080A02D9"),   # SaveReturnSuccessCallback|1
    ("0x0203B05C", "0x02022C30"),   # sWirelessLinkMain
    ("0x03005008", "0x03005D8C"),   # gSaveBlock1Ptr
    ("0x02036E6C", "0x02037384"),   # gObjectEvents[1].currentCoords.x
    ("0x02036E6E", "0x02037386"),
    ("0x02036E90", "0x020373A8"),   # gObjectEvents[2]
    ("0x02036E92", "0x020373AA"),
    ("0x02022B4C", "0x02022FEC"),   # gBattleTypeFlags
    ("0x03004FE0", "0x03005D60"),   # gBattlerControllerFuncs[0..3]
    ("0x03004FE4", "0x03005D64"),
    ("0x03004FE8", "0x03005D68"),
    ("0x03004FEC", "0x03005D6C"),
    ("0x0802E44D", "0x08057589"),   # HandleInputChooseAction|1
    ("0x0802EA25", "0x08057BFD"),   # HandleInputChooseMove|1
    ("0x02023FF8", "0x020244AC"),   # gActionSelectionCursor
    ("0x02023FF9", "0x020244AD"),
    ("0x02023FFA", "0x020244AE"),
    ("0x02023FFB", "0x020244AF"),
    ("0x02023FFC", "0x020244B0"),   # gMoveSelectionCursor
    ("0x02023FFD", "0x020244B1"),
    ("0x02023FFE", "0x020244B2"),
    ("0x02023FFF", "0x020244B3"),
    ("0x02023E8A", "0x0202433A"),   # gBattleOutcome
    ("0x03005AEE", "0x030059E6"),   # gRfu.recvQueue.count (HANDOVER §0)
    ("0x03005D22", "0x03005C1A"),   # gRfu.sendQueue.count
]
PP_FR, EP_FR, PP_E, EP_E = 0x02024284, 0x0202402C, 0x020244EC, 0x02024744
for base_fr, base_e in ((PP_FR, PP_E), (EP_FR, EP_E)):
    for i in range(2):
        for off in (0, 0x20, 0x38, 0x56):
            SUB.append(("0x%08X" % (base_fr + i * 100 + off), "0x%08X" % (base_e + i * 100 + off)))

BOOT = """# ---- P1-P3 boot (the proven Emerald tradecenter sequence) --------------
mash A 4 0x030022C4 0xFFFFFFFF 0x080AAB2D 4800
evt title_ok
mash START 4 0x030022C4 0xFFFFFFFF 0x0802F6B1 3600
mash A 4 0x030022C4 0xFFFFFFFF 0x08085E5D 4800
evt overworld_ok
wait 30
evt questlog_done

# ---- P4 golden save parks in Mauville PC 2F; the tradecenter route to the
# Direct Corner attendant (hardware-proven; its UP press relies on the
# Emerald overworld's one-tile holdmash overshoot, HARNESS §4)
logptr loc 2 0x03005D8C 0x04
holdmash DOWN  B 2 0x02037362 0xFFFF 12 900
holdmash RIGHT B 2 0x02037360 0xFFFF 17 900
holdmash UP    B 2 0x02037362 0xFFFF 11 900
press UP 2
waitram 1 0x02037368 0x0F 2 60
evt parked_ok

"""
for role in ("host", "join"):
    s = open(R + "frlg_battle_%s.inputs" % role, encoding="utf-8").read()
    a = s.index("# ---- P1-P3 boot")
    b = s.index("# ---- P5 talk")
    s = s[:a] + BOOT + s[b:]
    for x, y in SUB:
        s = s.replace(x, y)
    # service menu: TRADE/BATTLE/RECORD MIX/EXIT (Mauville visited, no Powder Jar: 4 rows); BATTLE is
    # row 1 in every variant, so wait for ANY menu and assert the cursor.
    s = s.replace("mashif A 1 0x020201CB 0xFF 1 1 0x0203CD94 0xFF 2 1800\nwaitram 1 0x0203CD92 0xFF 0 1\nevt service_menu",
                  "mashif A 1 0x020201CB 0xFF 1 1 0x0203CD94 0xFF 3 1800\nlogram svc_rows 1 0x0203CD94\nwaitram 1 0x0203CD92 0xFF 0 1\nevt service_menu")
    s = s.replace("# FR/LG WIRELESS DOUBLE BATTLE", "# EMERALD WIRELESS DOUBLE BATTLE (regression gate)")
    s = s.replace("# Addresses: pret/pokefirered `symbols` df4449a, pokefirered_rev1.sym and\n# pokeleafgreen_rev1.sym (identical for every address below).",
                  "# DERIVED from frlg_battle_%s.inputs by symbol substitution (pret/pokeemerald\n# `symbols`, pokeemerald.sym, BPEE rev 0); boot and walk are the proven\n# emerald_tradecenter sequence.  The FR/LG address table below is\n# superseded by the substituted values in the body." % role)
    open(R + "emerald_battle_%s.inputs" % role, "w", encoding="utf-8", newline="\n").write(s)
    left = [x for x, _ in SUB if x in s]
    print(role, "unsubstituted:", left)
