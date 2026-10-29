/* repair.h - parte "Recuperar cartão" da interface: diagnóstico, reconstrução de partições
 * e navegador/copiador de arquivos. Incluído por sdclone.c (usa as funções estáticas de lá). */
#ifndef REPAIR_H
#define REPAIR_H

#define WM_APP_ASK (WM_APP + 3)
#define WM_APP_COPYDONE (WM_APP + 4)

enum { RM_DIAG = 0, RM_BROWSE, RM_REBUILD, RM_CARVE, RM_RESTORE, RM_COUNT };

static Diag g_diag;           /* resultado do último diagnóstico (usado pelo navegador) */
static DiskInfo g_diagDisk;
static volatile LONG g_openBrowse = 0;

/* ------------------------------------------------------------ utilitários */
static int StrictRd(void *c, uint64_t off, void *buf, uint32_t len) { return ReadAtRaw((HANDLE)c, off, buf, len) ? 1 : 0; }

static void U8W(const char *s, wchar_t *o, int n) {
    int r = MultiByteToWideChar(CP_UTF8, 0, s, -1, o, n);
    if (!r) {
        int l = (int)strlen(s);
        if (l > n - 1) l = n - 1;
        int k = MultiByteToWideChar(CP_UTF8, 0, s, l, o, n - 1);
        o[k > 0 ? k : 0] = 0;
    }
    o[n - 1] = 0;
}

static void Cat(wchar_t *dst, size_t cap, const wchar_t *fmt, ...) {
    size_t l = wcslen(dst);
    if (l + 2 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(dst + l, cap - l - 1, fmt, ap);
    va_end(ap);
    dst[cap - 1] = 0;
}

static int DiagProgCb(void *c, uint64_t pos, int found) {
    static int last = -1;
    (void)c;
    g_done = (LONGLONG)pos;
    if (found != last) {
        wchar_t ph[160];
        last = found;
        _snwprintf(ph, 159, L"Procurando partições perdidas — %d encontrada(s)", found);
        ph[159] = 0;
        SetPhaseText(ph);
    }
    return g_cancel ? 1 : 0;
}

static void DescribePart(wchar_t *out, size_t cap, const Part *p, uint32_t sec) {
    wchar_t st[32], sz[32], lab[120], fsn[80];
    (void)sec;
    FmtSize(p->start, st, 32);
    FmtSize(p->size, sz, 32);
    lab[0] = 0;
    if (p->label[0]) {
        wchar_t t[100];
        U8W(p->label, t, 100);
        _snwprintf(lab, 119, L" \"%ls\"", t);
        lab[119] = 0;
    }
    if (p->flags & PF_EXTENDED) wcscpy(fsn, L"partição estendida (contêiner de partições lógicas)");
    else wcsncpy(fsn, FsNameW(p->fs), 79);
    fsn[79] = 0;
    _snwprintf(out, cap - 1, L"%ls%ls, tamanho %ls, início em %ls%ls%ls%ls", fsn, lab, sz, st,
               (p->flags & PF_BEYOND) ? L"  [ULTRAPASSA o fim do cartão]" : L"",
               (p->flags & PF_OVERLAP) ? L"  [sobreposta a outra]" : L"",
               (p->flags & PF_FSBIGGER) ? L"  [sistema de arquivos maior que a partição]" : L"");
    out[cap - 1] = 0;
}

/* --------------------------------------------------------------- relatório */
static void ReportDiag(const Diag *g, BOOL partial) {
    static const wchar_t *tn[] = {L"nenhuma (sem MBR/GPT válido)", L"MBR", L"GPT", L"nenhuma — o sistema de arquivos ocupa o cartão inteiro"};
    wchar_t cap[32], cov[32], rest[32], line[700], sm[2300];
    unsigned v = g->verdict;
    int nLinux = 0, nWinVis = 0;
    uint64_t maxEnd = 0;
    sm[0] = 0;
    FmtSize(g->diskBytes, cap, 32);
    FmtSize(g->coveredBytes, cov, 32);
    FmtSize(g->diskBytes > g->coveredBytes ? g->diskBytes - g->coveredBytes : 0, rest, 32);
    Log(L"=== DIAGNÓSTICO%ls ===", partial ? L" (busca interrompida)" : L"");
    Log(L"Capacidade que o cartão informa ao Windows: %ls (setor de %u bytes).", cap, g->sec);
    Log(L"Tabela de partições: %ls.", tn[g->tableType]);
    if (g->tableType == TBL_GPT)
        Log(L"GPT: cabeçalho %ls, checksums %ls, cópia de segurança %ls.", g->gptHeaderOk ? L"ok" : L"DANIFICADO", g->gptCrcOk ? L"ok" : L"INCORRETOS", g->gptBackupOk ? L"ok" : L"ausente/danificada");
    if (g->sampleFail) Log(L"Leituras de teste (começo, meio e fim): %d de %d FALHARAM.", g->sampleFail, g->sampleTotal);
    for (int i = 0; i < g->nparts; i++) {
        const Part *p = &g->parts[i];
        DescribePart(line, 700, p, g->sec);
        Log(L"  Partição %d: %ls", i + 1, line);
        if (p->start + p->size > maxEnd && (p->flags & PF_BEYOND)) maxEnd = p->start + p->size;
        if (!(p->flags & PF_EXTENDED)) {
            if (FsWindowsReadable(p->fs)) nWinVis++;
            if (p->fs == FST_EXT2 || p->fs == FST_EXT3 || p->fs == FST_EXT4) nLinux++;
        }
    }
    for (int i = 0; i < g->nfound; i++) {
        DescribePart(line, 700, &g->found[i], g->sec);
        Log(L"  Encontrada FORA da tabela %d: %ls", i + 1, line);
    }
    if (g->nparts == 0 && g->nfound == 0) Log(L"  Nenhuma partição nem sistema de arquivos reconhecido.");
    if (g->coveredBytes) Log(L"As partições cobrem %ls de %ls.", cov, cap);

    Log(L"--- O que isso significa ---");
    if (v & V_TINY) {
        Log(L"PROBLEMA: o cartão informa ao Windows que tem apenas %ls. Isso NÃO é falha de partição nem de formatação: é o próprio controlador do cartão respondendo uma capacidade errada. Acontece quando (1) o cartão é falso/adulterado, (2) o controlador entrou em modo de proteção por defeito ou desgaste (cartões que \"viram\" 8, 32 ou 64 MB), ou (3) o leitor/adaptador USB está com problema.", cap);
        Log(L"O que tentar: outro leitor, outra porta USB, e principalmente o slot SD do notebook. Se continuar igual, nenhum programa comum (incluindo este) consegue devolver a capacidade: só ferramentas de fábrica específicas do controlador (MPTool), que nem sempre existem. Nesse caso o cartão deve ser trocado e não deve guardar dados importantes.");
        Cat(sm, 2300, L"• O CARTÃO informa só %ls ao Windows (controlador com defeito, em proteção ou cartão falso). Não é problema de partição e não tem reparo por software comum. Teste outro leitor/slot; se igual, troque o cartão.\n\n", cap);
    }
    if (v & V_READERR) {
        Log(L"PROBLEMA: %d das %d leituras de teste falharam. Pode ser defeito físico no cartão, contatos sujos ou leitor ruim. Limpe os contatos, troque de leitor e, se persistir, copie o que for possível (Ver / copiar arquivos) antes que o cartão falhe de vez.", g->sampleFail, g->sampleTotal);
        Cat(sm, 2300, L"• Falhas de leitura (%d de %d testes): defeito físico, contatos sujos ou leitor ruim. Troque de leitor e salve os dados logo.\n\n", g->sampleFail, g->sampleTotal);
    }
    if (v & V_BEYOND) {
        wchar_t me[32];
        FmtSize(maxEnd, me, 32);
        if (v & V_TINY)
            Log(L"A tabela do cartão ainda descreve partições de até %ls, muito maiores que os %ls que o cartão informa agora: o cartão JÁ TEVE (ou finge ter) mais espaço. Os dados dessas partições estão fora do alcance.", me, cap);
        else
            Log(L"PROBLEMA: uma partição termina em %ls, além do fim do cartão (%ls). Causas: cartão FALSO (anuncia mais do que tem) ou imagem de um cartão maior gravada num menor. Rode a função Autenticidade para confirmar.", me, cap);
        Cat(sm, 2300, L"• Há partição que termina além do fim do cartão (%ls > %ls): típico de cartão falso ou de imagem de cartão maior. Rode Autenticidade.\n\n", me, cap);
    }
    if (v & V_SMALLPART) {
        int pct = g->diskBytes ? (int)(g->coveredBytes * 100 / g->diskBytes) : 0;
        Log(L"PROBLEMA: o cartão tem %ls, mas as partições cobrem só %ls (%d%%). O espaço restante (%ls) não pertence a nenhuma partição; por isso o Windows mostra só a parte pequena. É o caso típico de \"cartão de 128 GB que aparece com poucos MB/GB\": uma imagem pequena (sistema de console de 4 ou 8 GB) foi gravada num cartão maior e a tabela guarda só o tamanho da imagem. Seus dados estão intactos.", cap, cov, pct, rest);
        Log(L"Como resolver: SEM perder dados, amplie a última partição com um gerenciador de partições (GParted, MiniTool Partition Wizard, AOMEI). Para apagar tudo e usar o cartão inteiro: Restaurar o cartão (aqui). Para salvar os arquivos antes: Ver / copiar arquivos.");
        Cat(sm, 2300, L"• Este é o seu caso mais provável: o cartão tem %ls, mas as partições cobrem só %ls (%d%%). Os %ls restantes estão sem partição, por isso o Windows mostra pouco. Os dados estão intactos. Para recuperar o espaço: amplie a partição (GParted/MiniTool) ou use 'Restaurar o cartão' (apaga tudo).\n\n", cap, cov, pct, rest);
    }
    if (v & V_LINUX) {
        Log(L"Há partições Linux (ext2/3/4, swap, SquashFS...). O Windows NÃO consegue ler nem mostrar essas partições; por isso mostra só a pequena partição de boot (FAT) ou pede para formatar o cartão. Em consoles como R36S, ArkOS, JELOS, muOS isso é NORMAL. NÃO formate quando o Windows pedir. Use \"Ver / copiar arquivos\" para abrir as partições Linux aqui.");
        Cat(sm, 2300, L"• Há partições Linux (ext4 etc.) que o Windows não lê: é normal em consoles (R36S/ArkOS/JELOS). NÃO formate quando o Windows pedir; use 'Ver / copiar arquivos' aqui.\n\n");
    }
    if (v & V_ENCRYPTED) {
        Log(L"Há partição criptografada (BitLocker ou LUKS). Sem a senha/chave ela não pode ser lida por nenhum programa.");
        Cat(sm, 2300, L"• Há partição criptografada (BitLocker/LUKS): precisa da senha.\n\n");
    }
    if (v & V_NOTABLE) {
        Log(L"PROBLEMA: a tabela de partições está ausente ou apagada, mas foram encontradas %d partição(ões) com sistema de arquivos válido (acima). Seus dados provavelmente estão intactos. É por isso que o Windows diz que o cartão está \"RAW\" ou pede para formatar. NÃO formate: use Reconstruir partições (não apaga nada).", g->nfound);
        Cat(sm, 2300, L"• Tabela de partições apagada/corrompida, mas achei %d partição(ões) com dados intactos. Use 'Reconstruir partições' (não apaga nada) ou 'Ver / copiar arquivos'.\n\n", g->nfound);
    }
    if (v & V_RAW) {
        Log(L"PROBLEMA: não há tabela de partições nem sistema de arquivos reconhecível (cartão RAW, zerado ou com formato desconhecido). Se havia dados, tente \"Recuperar arquivos apagados\" (fotos, vídeos, PDF). Para voltar a usar o cartão: Restaurar o cartão (apaga tudo).");
        Cat(sm, 2300, L"• Cartão sem tabela de partições nem sistema de arquivos reconhecível (RAW/zerado). Para dados: 'Recuperar arquivos apagados'. Para usar de novo: 'Restaurar o cartão'.\n\n");
    }
    if (v & V_BADGPT) {
        Log(L"PROBLEMA: a tabela GPT está danificada. Este app não reconstrói GPT automaticamente. \"Ver / copiar arquivos\" lista o que for possível; para reparar use TestDisk ou gdisk, ou Restaurar o cartão (apaga tudo).");
        Cat(sm, 2300, L"• Tabela GPT danificada (o app não reconstrói GPT). Veja arquivos aqui, use TestDisk/gdisk, ou 'Restaurar o cartão'.\n\n");
    }
    if (v & V_SUPERFLOPPY) {
        Log(L"O cartão não tem tabela de partições: o sistema de arquivos ocupa o cartão inteiro (formato \"superfloppy\"). Alguns Windows/leitores não montam cartões assim. Os arquivos continuam acessíveis em \"Ver / copiar arquivos\". Para deixar o cartão normal: copie os arquivos e use Restaurar o cartão.");
        Cat(sm, 2300, L"• Cartão sem tabela de partições (formato 'superfloppy'): alguns leitores não o montam. Os arquivos estão acessíveis em 'Ver / copiar arquivos'.\n\n");
    }
    if (v & V_UNKNOWNPART) {
        Log(L"Há partição na tabela cujo sistema de arquivos não foi reconhecido (formato não suportado, vazio ou danificado).");
        Cat(sm, 2300, L"• Há partição com sistema de arquivos não reconhecido (danificado ou formato não suportado).\n\n");
    }
    if (v & V_HEALTHY) {
        Log(L"A estrutura do cartão está saudável: tabela de partições e sistemas de arquivos íntegros e capacidade coerente. Se o Windows não mostra o cartão, tente atribuir uma letra no Gerenciamento de Disco ou rode o chkdsk.");
        Cat(sm, 2300, L"Estrutura saudável: tabela de partições e sistemas de arquivos íntegros, capacidade coerente. Se o Windows não mostra o cartão, atribua uma letra no Gerenciamento de Disco ou rode o chkdsk.\n\n");
    }
    (void)nWinVis; (void)nLinux;
    if (!sm[0]) Cat(sm, 2300, L"Veja o registro para os detalhes.\n\n");
    SetSummary(MB_ICONINFORMATION, L"Capacidade informada pelo cartão: %ls\n\n%ls(Detalhes completos no registro da janela.)", cap, sm);
}

/* ------------------------------------------------------------- diagnóstico */
static BOOL DoDiagnose(Job *j, BOOL forBrowse) {
    BOOL ok = FALSE;
    HANDLE hd = OpenDisk(j->src, FALSE);
    Dev2 d;
    int haveDev = 0;
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão"); goto done; }
    if (!DiskLenMatches(hd, j->srcSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto done; }
    if (!Dev2Init(&d, StrictRd, hd, j->srcSize, j->srcSec)) { Log(L"ERRO: memória insuficiente."); goto done; }
    haveDev = 1;
    Log(L"Analisando o Disco %d (somente leitura): tabela de partições, sistemas de arquivos e partições perdidas...", j->src);
    SetPhase(L"Analisando o cartão...", j->srcSize);
    DiagAnalyze(&d, &g_diag, 1, DiagProgCb, NULL);
    g_diagDisk = j->di;
    ReportDiag(&g_diag, g_cancel != 0);
    if (forBrowse) {
        g_summary[0] = 0;
        g_openBrowse = 1;
    }
    ok = TRUE;
done:
    if (haveDev) Dev2Free(&d);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"Não foi possível analisar o cartão. Veja o registro.");
    return ok;
}

/* ------------------------------------------------- reconstrução de partições */
static BOOL SaveBackup(HANDLE hd, uint32_t sec, int disk, wchar_t *outPath) {
    wchar_t dir[MAX_PATH];
    SYSTEMTIME st;
    uint32_t len = 1024 * 1024;
    uint8_t *buf = (uint8_t *)ALLOC_ALIGNED(len);
    BOOL ok = FALSE;
    if (!buf) return FALSE;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, dir))) GetTempPathW(MAX_PATH, dir);
    else wcscat(dir, L"\\");
    GetLocalTime(&st);
    _snwprintf(outPath, MAX_PATH - 1, L"%lsSDCLONETOOLS_backup_Disco%d_%04d%02d%02d_%02d%02d%02d.bin", dir, disk, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    outPath[MAX_PATH - 1] = 0;
    if (!ReadAtRaw(hd, 0, buf, len)) { len = sec; if (!ReadAtRaw(hd, 0, buf, len)) goto done; }
    HANDLE hf = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) goto done;
    DWORD w = 0;
    ok = WriteFile(hf, buf, len, &w, NULL) && w == len;
    CloseHandle(hf);
done:
    FREE_ALIGNED(buf);
    return ok;
}

static BOOL DoRebuild(Job *j) {
    BOOL ok = FALSE;
    HANDLE hd = OpenDisk(j->src, FALSE), hw = INVALID_HANDLE_VALUE;
    Dev2 d;
    int haveDev = 0;
    VolSet vs;
    Diag *g = &g_diag;
    Part list[8];
    int dropped = 0, n;
    uint8_t *s0 = NULL, *old = NULL;
    wchar_t msg[1800], one[260], bak[MAX_PATH];
    vs.n = 0;
    if (j->src == g_sysDisk) { Log(L"ERRO: disco do sistema — recusado."); goto done; }
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão"); goto done; }
    if (!DiskLenMatches(hd, j->srcSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto done; }
    if (!Dev2Init(&d, StrictRd, hd, j->srcSize, j->srcSec)) { Log(L"ERRO: memória insuficiente."); goto done; }
    haveDev = 1;
    Log(L"Procurando partições do Disco %d para reconstruir a tabela (somente leitura até a confirmação)...", j->src);
    SetPhase(L"Analisando o cartão...", j->srcSize);
    DiagAnalyze(&d, g, 1, DiagProgCb, NULL);
    if (g_cancel) { Log(L"Cancelado. Nada foi alterado."); SetSummary(MB_ICONINFORMATION, L"Cancelado. Nada foi alterado no cartão."); goto done; }
    ReportDiag(g, FALSE);
    g_summary[0] = 0;
    if (g->tableType == TBL_GPT || g->tableType == TBL_SUPERFLOPPY) {
        Log(L"Este cartão usa %ls: o app só reconstrói tabelas MBR. Nada foi alterado.", g->tableType == TBL_GPT ? L"GPT" : L"formato sem tabela (superfloppy)");
        SetSummary(MB_ICONWARNING, L"Este cartão usa %ls. O app só reconstrói tabelas MBR, então nada foi alterado.\n\nUse 'Ver / copiar arquivos' para salvar os dados, ou TestDisk/gdisk para reparar.", g->tableType == TBL_GPT ? L"GPT" : L"um formato sem tabela de partições");
        goto done;
    }
    n = BuildRecoveryList(g, list, 8, &dropped);
    if (n <= 0) {
        Log(L"Nenhuma partição encontrada para reconstruir. Nada foi alterado.");
        SetSummary(MB_ICONWARNING, L"Não encontrei nenhuma partição com sistema de arquivos válido para reconstruir. Nada foi alterado.\n\nTente 'Recuperar arquivos apagados' ou 'Restaurar o cartão'.");
        goto done;
    }
    /* a tabela atual já é igual? */
    {
        int same = (g->tableType == TBL_MBR && g->nfound == 0);
        if (same) {
            int cnt = 0;
            for (int i = 0; i < g->nparts; i++) if (g->parts[i].src == SRC_MBR && !(g->parts[i].flags & PF_EXTENDED)) cnt++;
            same = (cnt == n && !(g->verdict & (V_BEYOND | V_UNKNOWNPART)));
        }
        if (same) {
            Log(L"A tabela de partições já descreve as partições encontradas. Nada a reconstruir.");
            SetSummary(MB_ICONINFORMATION, L"A tabela de partições já está correta. Nada foi alterado.\n\nSe o espaço continua pequeno, veja o diagnóstico: o problema não é a tabela.");
            goto done;
        }
    }
    msg[0] = 0;
    Cat(msg, 1800, L"A tabela de partições do cartão será reescrita com %d partição(ões):\n\n", n);
    for (int i = 0; i < n; i++) {
        DescribePart(one, 260, &list[i], g->sec);
        Cat(msg, 1800, L"%d) %ls\n", i + 1, one);
    }
    if (dropped) Cat(msg, 1800, L"\n(%d partição(ões) a mais foram encontradas, mas o MBR só comporta 4.)\n", dropped);
    Cat(msg, 1800, L"\nOs DADOS não são apagados, só o setor de partições é regravado. Uma cópia do início do cartão é salva em Documentos antes. Continuar?");
    if (SendMessageW(g_hwnd, WM_APP_ASK, 0, (LPARAM)msg) != IDYES) {
        Log(L"Reconstrução cancelada pelo usuário. Nada foi alterado.");
        SetSummary(MB_ICONINFORMATION, L"Cancelado. Nada foi alterado no cartão.");
        goto done;
    }
    old = (uint8_t *)ALLOC_ALIGNED(g->sec);
    s0 = (uint8_t *)ALLOC_ALIGNED(g->sec);
    if (!old || !s0) { Log(L"ERRO: memória insuficiente."); goto done; }
    if (!ReadAtRaw(hd, 0, old, g->sec)) { LogErr(L"ler o setor 0"); goto done; }
    if (!SaveBackup(hd, g->sec, j->src, bak)) { Log(L"ERRO: não foi possível salvar a cópia de segurança do início do cartão. Nada foi alterado."); SetSummary(MB_ICONERROR, L"Não foi possível salvar a cópia de segurança em Documentos. Por segurança, nada foi alterado."); goto done; }
    Log(L"Cópia de segurança do início do cartão: %ls", bak);
    if (BuildMbr(s0, g->sec, old, list, n) != 0) { Log(L"ERRO: as partições não cabem em um MBR (acima de 2 TB ou inválidas)."); goto done; }
    if (!LockDiskVolumes(j->src, &vs)) goto done;
    hw = OpenDisk(j->src, TRUE);
    if (hw == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão para escrita"); goto done; }
    if (!WriteAtRaw(hw, 0, s0, g->sec)) { LogErr(L"gravar a tabela de partições"); goto done; }
    FlushFileBuffers(hw);
    UpdateDiskProps(hw);
    ok = TRUE;
    Log(L"Tabela de partições reconstruída com %d partição(ões). Remova e reinsira o cartão se ele não aparecer.", n);
    SetSummary(MB_ICONINFORMATION, L"Tabela de partições reconstruída (%d partição(ões)).\n\nRemova e reinsira o cartão. Se o Windows pedir para formatar, clique em CANCELAR e use 'Ver / copiar arquivos' para conferir.\n\nCópia de segurança do início do cartão:\n%ls", n, bak);
done:
    if (hw != INVALID_HANDLE_VALUE) CloseHandle(hw);
    UnlockVolumes(&vs);
    if (old) FREE_ALIGNED(old);
    if (s0) FREE_ALIGNED(s0);
    if (haveDev) Dev2Free(&d);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"A reconstrução não foi concluída. Veja o registro.");
    return ok;
}

/* ============================ NAVEGADOR DE ARQUIVOS ============================ */

typedef struct { int src; uint64_t start; int fs; wchar_t text[220]; } BrOpt;

typedef struct {
    HWND hw;
    BOOL alive, copying;
    HANDLE hd;
    Dev2 d;
    int devOk;
    Fs fs;
    int fsOk;
    int disk;
    BrOpt opts[MAXP + MAXF];
    int nopts, curOpt;
    FsEntry stack[48];
    int depth;
    FsEntry *items;
    int nitems;
    /* cópia */
    HANDLE thread;
    wchar_t dest[MAX_PATH];
    int *sel, nsel;
    volatile LONG cancel;
    int files, dirs, fail, skipped;
    uint64_t bytes;
    ULONGLONG lastUi;
    wchar_t cur[260];
} Br;
static Br *B;

static int EntCmp(const void *a, const void *b) {
    const FsEntry *x = (const FsEntry *)a, *y = (const FsEntry *)b;
    int dx = (x->kind == K_DIR), dy = (y->kind == K_DIR);
    if (dx != dy) return dy - dx;
    return _stricmp(x->name, y->name);
}

static void BrSetInfo(const wchar_t *t) { SetWindowTextW(GetDlgItem(B->hw, ID_BR_INFO), t); }

static void BrPathText(void) {
    wchar_t p[1200], t[300];
    p[0] = 0;
    for (int i = 1; i <= B->depth; i++) {
        U8W(B->stack[i].name, t, 300);
        wcsncat(p, L"/", 1199 - wcslen(p));
        wcsncat(p, t, 1199 - wcslen(p));
    }
    if (!p[0]) wcscpy(p, L"/");
    wchar_t o[1300];
    _snwprintf(o, 1299, L"Pasta atual: %ls", p);
    o[1299] = 0;
    SetWindowTextW(GetDlgItem(B->hw, ID_BR_PATH), o);
}

static void BrLoadDir(void) {
    HWND lv = GetDlgItem(B->hw, ID_BR_LIST);
    wchar_t info[200];
    if (B->items) { free(B->items); B->items = NULL; }
    B->nitems = 0;
    if (!B->fsOk) { ListView_SetItemCountEx(lv, 0, 0); BrPathText(); return; }
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (FsList(&B->fs, &B->stack[B->depth], &B->items, &B->nitems) != 0) {
        B->items = NULL; B->nitems = 0;
        BrSetInfo(L"Não foi possível ler esta pasta (estrutura danificada ou setores ilegíveis).");
    } else {
        qsort(B->items, (size_t)B->nitems, sizeof(FsEntry), EntCmp);
        _snwprintf(info, 199, L"%d item(ns). Dê dois cliques numa pasta para abrir. Selecione itens e use \"Copiar selecionados\".", B->nitems);
        info[199] = 0;
        BrSetInfo(info);
    }
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    ListView_SetItemCountEx(lv, B->nitems, 0);
    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
    InvalidateRect(lv, NULL, TRUE);
    BrPathText();
    EnableWindow(GetDlgItem(B->hw, ID_BR_UP), B->depth > 0);
}

static void BrOpenPartition(int idx) {
    if (B->fsOk) { FsClose(&B->fs); B->fsOk = 0; }
    B->curOpt = idx;
    B->depth = 0;
    B->d.cLen = 0;
    if (idx >= 0 && idx < B->nopts) {
        int t = FsOpen(&B->fs, &B->d, B->opts[idx].start);
        if (t) {
            B->fsOk = 1;
            FsRoot(&B->fs, &B->stack[0]);
        } else {
            BrSetInfo(L"Não foi possível abrir o sistema de arquivos desta partição (danificado ou formato não suportado).");
        }
    }
    BrLoadDir();
    if (!B->fsOk) BrSetInfo(L"Não foi possível abrir o sistema de arquivos desta partição (danificado ou formato não suportado).");
}

static void BrEnter(int row) {
    if (row < 0 || row >= B->nitems || B->copying) return;
    if (B->items[row].kind != K_DIR || B->depth >= 46) return;
    B->stack[B->depth + 1] = B->items[row];
    B->depth++;
    BrLoadDir();
}

static void BrUp(void) {
    if (B->depth > 0 && !B->copying) { B->depth--; BrLoadDir(); }
}

/* --------------------------------------------------------------------- cópia */
static void SanitizeName(wchar_t *s) {
    for (wchar_t *p = s; *p; p++) if (*p < 32 || wcschr(L"<>:\"/\\|?*", *p)) *p = L'_';
    size_t l = wcslen(s);
    while (l > 0 && (s[l - 1] == L'.' || s[l - 1] == L' ')) s[--l] = 0;
    if (!l) { wcscpy(s, L"_"); return; }
    static const wchar_t *res[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"LPT1", L"LPT2", L"LPT3"};
    wchar_t base[16];
    size_t k = 0;
    while (s[k] && s[k] != L'.' && k < 15) { base[k] = s[k]; k++; }
    base[k] = 0;
    for (int i = 0; i < 11; i++) if (!_wcsicmp(base, res[i]) && (s[k] == 0 || s[k] == L'.')) { memmove(s + 1, s, (wcslen(s) + 1) * sizeof(wchar_t)); s[0] = L'_'; break; }
}

static void BrProgress(BOOL force) {
    ULONGLONG t = GetTickCount64();
    if (!force && t - B->lastUi < 200) return;
    B->lastUi = t;
    wchar_t sz[32], m[500];
    FmtSize(B->bytes, sz, 32);
    _snwprintf(m, 499, L"Copiando… %d arquivo(s), %ls. Atual: %ls", B->files, sz, B->cur);
    m[499] = 0;
    BrSetInfo(m);
}

static int CopySink(void *ctx, const uint8_t *p, uint32_t n) {
    HANDLE h = (HANDLE)ctx;
    DWORD w = 0;
    if (B->cancel) return 1;
    if (!WriteFile(h, p, n, &w, NULL) || w != n) return -1;
    B->bytes += n;
    BrProgress(FALSE);
    return 0;
}

static void CopyEntry(const FsEntry *e, const wchar_t *dir, int depth) {
    wchar_t nm[300], full[MAX_PATH];
    if (B->cancel) return;
    U8W(e->name, nm, 300);
    SanitizeName(nm);
    if (e->kind == K_DIR) {
        if (depth > 40) return;
        _snwprintf(full, MAX_PATH - 1, L"%ls\\%ls", dir, nm);
        full[MAX_PATH - 1] = 0;
        if (!CreateDirectoryW(full, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            Log(L"ERRO: não foi possível criar a pasta %ls", full);
            B->fail++;
            return;
        }
        FsEntry *arr = NULL;
        int n = 0;
        if (FsList(&B->fs, e, &arr, &n) != 0) {
            Log(L"ERRO: não foi possível ler a pasta \"%ls\" do cartão.", nm);
            B->fail++;
            return;
        }
        for (int i = 0; i < n && !B->cancel; i++) CopyEntry(&arr[i], full, depth + 1);
        free(arr);
        B->dirs++;
    } else if (e->kind == K_FILE) {
        HANDLE hf = INVALID_HANDLE_VALUE;
        for (int k = 0; k < 1000; k++) {
            if (k == 0) _snwprintf(full, MAX_PATH - 1, L"%ls\\%ls", dir, nm);
            else {
                wchar_t *dot = wcsrchr(nm, L'.');
                if (dot && dot != nm) _snwprintf(full, MAX_PATH - 1, L"%ls\\%.*ls (%d)%ls", dir, (int)(dot - nm), nm, k + 1, dot);
                else _snwprintf(full, MAX_PATH - 1, L"%ls\\%ls (%d)", dir, nm, k + 1);
            }
            full[MAX_PATH - 1] = 0;
            hf = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) break;
            DWORD er = GetLastError();
            if (er != ERROR_FILE_EXISTS && er != ERROR_ALREADY_EXISTS) break;
        }
        if (hf == INVALID_HANDLE_VALUE) {
            Log(L"ERRO: não foi possível criar %ls (caminho longo demais ou sem permissão).", full);
            B->fail++;
            return;
        }
        wcsncpy(B->cur, nm, 259);
        B->cur[259] = 0;
        int rc = FsStream(&B->fs, e, CopySink, hf);
        CloseHandle(hf);
        if (rc == 0) B->files++;
        else if (rc == 1) { /* cancelado */ DeleteFileW(full); }
        else {
            Log(L"ERRO ao copiar \"%ls\": dados ilegíveis ou estrutura danificada (arquivo incompleto mantido).", nm);
            B->fail++;
        }
    } else {
        B->skipped++;
    }
}

static DWORD WINAPI CopyThread(LPVOID p) {
    (void)p;
    for (int i = 0; i < B->nsel && !B->cancel; i++) CopyEntry(&B->items[B->sel[i]], B->dest, 0);
    BrProgress(TRUE);
    PostMessageW(B->hw, WM_APP_COPYDONE, 0, 0);
    return 0;
}

static void BrSetCopying(BOOL c) {
    B->copying = c;
    EnableWindow(GetDlgItem(B->hw, ID_BR_PART), !c);
    EnableWindow(GetDlgItem(B->hw, ID_BR_LIST), !c);
    EnableWindow(GetDlgItem(B->hw, ID_BR_COPYSEL), !c);
    EnableWindow(GetDlgItem(B->hw, ID_BR_COPYALL), !c);
    EnableWindow(GetDlgItem(B->hw, ID_BR_UP), !c && B->depth > 0);
    EnableWindow(GetDlgItem(B->hw, ID_BR_CLOSE), !c);
    EnableWindow(GetDlgItem(B->hw, ID_BR_CANCEL), c);
}

static void BrCopy(BOOL all) {
    HWND lv = GetDlgItem(B->hw, ID_BR_LIST);
    if (B->copying || !B->fsOk) return;
    int *sel = (int *)malloc(sizeof(int) * (size_t)(B->nitems > 0 ? B->nitems : 1));
    int n = 0;
    if (!sel) return;
    if (all) { for (int i = 0; i < B->nitems; i++) sel[n++] = i; }
    else {
        int i = -1;
        while ((i = ListView_GetNextItem(lv, i, LVNI_SELECTED)) >= 0 && i < B->nitems) sel[n++] = i;
    }
    if (!n) {
        free(sel);
        MessageBoxW(B->hw, all ? L"Esta pasta está vazia." : L"Selecione um ou mais itens na lista (use Ctrl ou Shift para vários).", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = B->hw;
    bi.lpszTitle = L"Escolha a pasta de destino (NÃO use o próprio cartão)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) { free(sel); return; }
    wchar_t out[MAX_PATH];
    BOOL got = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    if (!got) { free(sel); return; }
    if (PathOnDisk(out, B->disk)) {
        free(sel);
        MessageBoxW(B->hw, L"Escolha uma pasta em outro disco, nunca no próprio cartão.", APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }
    size_t l = wcslen(out);
    while (l > 3 && out[l - 1] == L'\\') out[--l] = 0;
    wcscpy(B->dest, out);
    if (B->sel) free(B->sel);
    B->sel = sel;
    B->nsel = n;
    B->cancel = 0; B->files = B->dirs = B->fail = B->skipped = 0; B->bytes = 0; B->cur[0] = 0;
    Log(L"Copiando %d item(ns) do cartão para %ls ...", n, B->dest);
    BrSetCopying(TRUE);
    B->thread = CreateThread(NULL, 0, CopyThread, NULL, 0, NULL);
    if (!B->thread) { BrSetCopying(FALSE); MessageBoxW(B->hw, L"Não foi possível iniciar a cópia.", APP_TITLE, MB_OK | MB_ICONERROR); }
}

static void BrCopyDone(void) {
    wchar_t sz[32], m[800];
    if (B->thread) { WaitForSingleObject(B->thread, 5000); CloseHandle(B->thread); B->thread = NULL; }
    FmtSize(B->bytes, sz, 32);
    BrSetCopying(FALSE);
    Log(L"Cópia %ls: %d arquivo(s), %d pasta(s), %ls; %d falha(s), %d item(ns) ignorado(s) (links/especiais).", B->cancel ? L"cancelada" : L"concluída", B->files, B->dirs, sz, B->fail, B->skipped);
    _snwprintf(m, 799, L"Cópia %ls.\n\n%d arquivo(s) e %d pasta(s) copiados (%ls).\n%d falha(s). %d item(ns) ignorado(s) (links simbólicos/arquivos especiais).\n\nDestino:\n%ls", B->cancel ? L"cancelada" : L"concluída", B->files, B->dirs, sz, B->fail, B->skipped, B->dest);
    m[799] = 0;
    MessageBoxW(B->hw, m, APP_TITLE, MB_OK | (B->fail ? MB_ICONWARNING : MB_ICONINFORMATION));
    BrLoadDir();
}

/* ------------------------------------------------------------------ janela */
static LRESULT CALLBACK BrProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ID_BR_PART && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(GetDlgItem(h, ID_BR_PART), CB_GETCURSEL, 0, 0);
            if (i >= 0) BrOpenPartition(i);
        } else if (id == ID_BR_UP && code == BN_CLICKED) BrUp();
        else if (id == ID_BR_COPYSEL && code == BN_CLICKED) BrCopy(FALSE);
        else if (id == ID_BR_COPYALL && code == BN_CLICKED) BrCopy(TRUE);
        else if (id == ID_BR_CANCEL && code == BN_CLICKED) { B->cancel = 1; BrSetInfo(L"Cancelando..."); }
        else if (id == ID_BR_CLOSE && code == BN_CLICKED) SendMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->idFrom == ID_BR_LIST) {
            if (nh->code == LVN_GETDISPINFOW) {
                NMLVDISPINFOW *di = (NMLVDISPINFOW *)l;
                static wchar_t buf[8][420];
                static int bi = 0;
                if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 && di->item.iItem < B->nitems) {
                    const FsEntry *e = &B->items[di->item.iItem];
                    wchar_t *b = buf[bi++ & 7];
                    b[0] = 0;
                    if (di->item.iSubItem == 0) U8W(e->name, b, 420);
                    else if (di->item.iSubItem == 1) wcscpy(b, e->kind == K_DIR ? L"Pasta" : (e->kind == K_FILE ? L"Arquivo" : (e->kind == K_LINK ? L"Link" : L"Especial")));
                    else if (e->kind == K_FILE) FmtSize(e->size, b, 40);
                    di->item.pszText = b;
                }
                return 0;
            }
            if (nh->code == NM_DBLCLK || nh->code == NM_RETURN) {
                int row = (nh->code == NM_DBLCLK) ? ((NMITEMACTIVATE *)l)->iItem : ListView_GetNextItem(GetDlgItem(h, ID_BR_LIST), -1, LVNI_SELECTED);
                BrEnter(row);
                return 0;
            }
        }
        break;
    }
    case WM_APP_COPYDONE:
        BrCopyDone();
        return 0;
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT rc;
        GetClientRect(h, &rc);
        Fill(dc, rc, CL_BG);
        static const int fr[] = {ID_BR_PART, ID_BR_LIST};
        for (int i = 0; i < 2; i++) {
            RECT r;
            HWND c = GetDlgItem(h, fr[i]);
            if (!c) continue;
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, h, (POINT *)&r, 2);
            InflateRect(&r, 1, 1);
            Frame(dc, r, CL_EDGE);
        }
        return 1;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
        if (d->CtlType == ODT_BUTTON) DrawButton(d);
        else if (d->CtlType == ODT_COMBOBOX) DrawCombo(d);
        return TRUE;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = S(20); return TRUE; }
        break;
    }
    case WM_CTLCOLORLISTBOX: SetTextColor((HDC)w, CL_TXT); SetBkColor((HDC)w, CL_INPUT); return (LRESULT)g_brInput;
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)l);
        SetTextColor((HDC)w, id == ID_BR_INFO ? CL_DIM : (id == ID_BR_PATH ? CL_NEON : CL_TXT));
        SetBkColor((HDC)w, CL_BG);
        return (LRESULT)g_brBg;
    }
    case WM_CLOSE:
        if (B->copying) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        B->alive = FALSE;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND bmk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), B->hw, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void BrAddOpt(const Part *p, int idx, const Diag *g) {
    wchar_t sz[32], lab[100], t[60];
    if (p->flags & PF_EXTENDED) return;
    if (!FsBrowsable(p->fs) || p->start >= g->diskBytes) return;
    BrOpt *o = &B->opts[B->nopts++];
    o->src = p->src; o->start = p->start; o->fs = p->fs;
    FmtSize(p->size ? p->size : p->fsBytes, sz, 32);
    lab[0] = 0;
    if (p->label[0]) { U8W(p->label, t, 60); _snwprintf(lab, 99, L" \"%ls\"", t); lab[99] = 0; }
    if (p->src == SRC_FOUND) _snwprintf(o->text, 219, L"Encontrada fora da tabela — %ls%ls — %ls", FsNameW(p->fs), lab, sz);
    else _snwprintf(o->text, 219, L"Partição %d — %ls%ls — %ls%ls", idx + 1, FsNameW(p->fs), lab, sz, FsWindowsReadable(p->fs) ? L"" : L"  (o Windows NÃO vê esta)");
    o->text[219] = 0;
}

static void ShowBrowser(void) {
    const Diag *g = &g_diag;
    static BOOL reg = FALSE;
    Br br;
    memset(&br, 0, sizeof br);
    B = &br;
    br.disk = g_diagDisk.index;
    for (int i = 0; i < g->nparts; i++) BrAddOpt(&g->parts[i], i, g);
    for (int i = 0; i < g->nfound; i++) BrAddOpt(&g->found[i], i, g);
    if (!br.nopts) {
        MessageBoxW(g_hwnd, L"Não encontrei nenhuma partição que este app consiga abrir (suporta FAT12/16/32, exFAT e ext2/3/4).\n\nVeja o diagnóstico no registro. Se o cartão está zerado/RAW, tente 'Recuperar arquivos apagados'.", APP_TITLE, MB_OK | MB_ICONWARNING);
        B = NULL;
        return;
    }
    br.hd = OpenDisk(br.disk, FALSE);
    if (br.hd == INVALID_HANDLE_VALUE || !Dev2Init(&br.d, StrictRd, br.hd, g_diagDisk.size, g_diagDisk.sec)) {
        MessageBoxW(g_hwnd, L"Não foi possível abrir o cartão para leitura.", APP_TITLE, MB_OK | MB_ICONERROR);
        if (br.hd != INVALID_HANDLE_VALUE) CloseHandle(br.hd);
        B = NULL;
        return;
    }
    br.devOk = 1;
    if (!reg) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = BrProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = g_brBg;
        wc.lpszClassName = L"SDBrowseWnd";
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        reg = TRUE;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = {0, 0, S(760), S(520)};
    AdjustWindowRect(&rc, style, FALSE);
    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    br.hw = CreateWindowExW(0, L"SDBrowseWnd", L"SDCLONETOOLS — Ver / copiar arquivos do cartão (somente leitura)", style,
                            pr.left + 20, pr.top + 20, ww, wh, g_hwnd, NULL, g_inst, NULL);
    if (!br.hw) { Dev2Free(&br.d); CloseHandle(br.hd); B = NULL; return; }
    br.alive = TRUE;
    DarkTitlebar(br.hw);
    bmk(L"STATIC", L"Partição do cartão:", SS_LEFT, 10, 8, 400, 18, ID_BR_LBL);
    HWND cb = bmk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 10, 28, 740, 240, ID_BR_PART);
    SetTheme(cb, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    bmk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 10, 62, 740, 18, ID_BR_PATH);
    HWND lv = bmk(L"SysListView32", L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, 10, 86, 740, 330, ID_BR_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(lv, CL_INPUT);
    ListView_SetTextBkColor(lv, CL_INPUT);
    ListView_SetTextColor(lv, CL_TXT);
    SetTheme(lv, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    HWND hdr = ListView_GetHeader(lv);
    if (hdr) SetTheme(hdr, g_dark ? L"DarkMode_ItemsView" : L"ItemsView", NULL);
    LVCOLUMNW col;
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = L"Nome"; col.cx = S(450); SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = L"Tipo"; col.cx = S(90); SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = L"Tamanho"; col.cx = S(160); SendMessageW(lv, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    bmk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 10, 424, 740, 20, ID_BR_INFO);
    HWND b1 = bmk(L"BUTTON", L"↑ Subir", BS_OWNERDRAW | WS_TABSTOP, 10, 458, 90, 36, ID_BR_UP);
    HWND b2 = bmk(L"BUTTON", L"Copiar selecionados", BS_OWNERDRAW | WS_TABSTOP, 110, 458, 190, 36, ID_BR_COPYSEL);
    HWND b3 = bmk(L"BUTTON", L"Copiar tudo desta pasta", BS_OWNERDRAW | WS_TABSTOP, 310, 458, 200, 36, ID_BR_COPYALL);
    HWND b4 = bmk(L"BUTTON", L"Cancelar cópia", BS_OWNERDRAW | WS_TABSTOP | WS_DISABLED, 520, 458, 120, 36, ID_BR_CANCEL);
    HWND b5 = bmk(L"BUTTON", L"Fechar", BS_OWNERDRAW | WS_TABSTOP, 650, 458, 100, 36, ID_BR_CLOSE);
    HWND bs[] = {b1, b2, b3, b4, b5};
    for (int i = 0; i < 5; i++) SetWindowSubclass(bs[i], BtnSub, (UINT_PTR)GetDlgCtrlID(bs[i]), 0);
    for (int i = 0; i < br.nopts; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)br.opts[i].text);
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
    BrOpenPartition(0);
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(br.hw, SW_SHOW);
    UpdateWindow(br.hw);
    MSG msg;
    while (br.alive && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(br.hw, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    if (br.fsOk) FsClose(&br.fs);
    if (br.items) free(br.items);
    if (br.sel) free(br.sel);
    Dev2Free(&br.d);
    CloseHandle(br.hd);
    B = NULL;
}

#endif
