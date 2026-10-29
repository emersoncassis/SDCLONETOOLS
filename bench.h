/* bench.h - julgamento do teste de desempenho (puro, sem Windows).
 * Os limites são heurísticas medidas com acesso sequencial e aleatório de 4 KB uma operação por vez (fila 1),
 * por isso ficam abaixo dos números de catálogo das classes A1/A2. */
#ifndef BENCH_H
#define BENCH_H

typedef struct {
    double seqR, seqW;      /* MB/s */
    double rMin, rMax;      /* menor e maior velocidade de leitura entre início, meio e fim (MB/s) */
    double wMed, wMin;      /* mediana e menor janela da gravação sequencial (MB/s) */
    double rr, rw;          /* operações de 4 KB por segundo */
    int hasW;               /* gravação medida */
    int rErr, wErr;         /* erros de leitura / gravação / dados que voltaram diferentes */
} BenchRes;

enum { BQ_POOR = 0, BQ_FAIR = 1, BQ_GOOD = 2 };
enum { BT_BAD = 0, BT_DOUBT = 1, BT_OK = 2 };

typedef struct {
    int classIdx;           /* 0 = abaixo de Class 2, 1 C2, 2 C4, 3 C6, 4 C10/U1, 5 U3/V30, 6 V60, 7 V90; -1 = não medido */
    int rand;               /* acesso aleatório de leitura: BQ_* */
    int randW;              /* acesso aleatório de gravação: BQ_* (ou -1 se não medido) */
    int game;               /* 0 fraco, 1 serve para jogos leves, 2 bom */
    int media;              /* 0 não serve, 1 fotos e vídeo básico, 2 Full HD, 3 4K; -1 não testado */
    int trust;              /* BT_* */
} BenchJudge;

static const char *BENCH_CLASS_NAME[] = {"abaixo de Class 2", "Class 2", "Class 4", "Class 6", "Class 10 / U1", "U3 / V30", "V60", "V90"};

static void BenchEval(const BenchRes *r, BenchJudge *o) {
    o->classIdx = -1;
    if (r->hasW) {
        double w = r->seqW;
        o->classIdx = w >= 90 ? 7 : w >= 60 ? 6 : w >= 30 ? 5 : w >= 10 ? 4 : w >= 6 ? 3 : w >= 4 ? 2 : w >= 2 ? 1 : 0;
    }
    o->rand = r->rr >= 1000 ? BQ_GOOD : (r->rr >= 400 ? BQ_FAIR : BQ_POOR);
    o->randW = r->hasW ? (r->rw >= 300 ? BQ_GOOD : (r->rw >= 100 ? BQ_FAIR : BQ_POOR)) : -1;
    o->game = (r->seqR >= 40 && r->rr >= 1000) ? 2 : ((r->seqR >= 20 && r->rr >= 400) ? 1 : 0);
    if (!r->hasW) o->media = -1;
    else o->media = r->seqW >= 30 ? 3 : (r->seqW >= 10 ? 2 : (r->seqW >= 4 ? 1 : 0));
    /* confiança */
    o->trust = BT_OK;
    if (r->rErr > 0 || r->wErr > 0 || r->seqR < 5 || (r->hasW && r->seqW < 2)) o->trust = BT_BAD;
    else {
        int doubt = 0;
        if (r->rMin > 0 && r->rMax / r->rMin > 3.0) doubt = 1;                    /* leitura muito desigual entre regiões */
        if (r->hasW && r->wMed > 0 && r->wMin < r->wMed * 0.1) doubt = 1;         /* quedas bruscas na gravação */
        if (r->hasW && o->randW == BQ_POOR && o->rand == BQ_POOR) doubt = 1;
        if (doubt) o->trust = BT_DOUBT;
    }
}

#endif
