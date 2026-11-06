"""Keep strict same-mode FIFO through peer failures, then drain every retained scene."""
from pathlib import Path
import subprocess,tempfile,argparse
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--baseline-dir',type=Path)
args=parser.parse_args()
out=root/'build-host-checks/sd-replay-fairness';out.mkdir(parents=True,exist_ok=True)
catalog=(args.baseline_dir/'main/sd_audio_catalog.c') if args.baseline_dir else root/'main/sd_audio_catalog.c'
(out/'sd_audio_catalog.c').write_text(catalog.read_text(encoding='utf-8'),encoding='utf-8')
stub=root/'main/audio_modes/meeting/host_stubs'
def function(source, signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
def run(name,source):
    src=out/(name+'.c');exe=out/(name+'.exe');src.write_text(source,encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
        '-I'+str(out),'-I'+str(stub),'-I'+str(root/'main'),str(src),'-o',str(exe)],check=True)
    for failures in [17,65,200]:
        for mode in [0,1]:
            for order in [0,1,2,3]:
                with tempfile.TemporaryDirectory(prefix='nuwax-fairness-') as directory:
                    result=subprocess.run([str(exe),str(failures),str(mode),str(order)],cwd=directory,
                                          capture_output=True,text=True)
                    if result.returncode:
                        print(result.stdout,result.stderr,flush=True)
                        result.check_returncode()
                    print(result.stdout.splitlines()[-1])
fixture = (root/'main/sd_audio_recovery_host_check.c').read_text(encoding='utf-8')
prefix = fixture[:fixture.index('int main(void)')]
old = function(prefix, 'static esp_err_t replay_one_ready(')
prefix = prefix.replace(old, r'''
static bool peer_busy=true;
static esp_err_t replay_one_ready(nuwax_ws_client_t *ws, uint32_t epoch,
    const char *path, bool independent) {
    (void)ws; (void)epoch; (void)independent; assert(!held);
    if (strstr(path, "zz_good_last")) {
        new_attempts++; return sd_audio_catalog_ack("zz_good_last", 0);
    }
    old_attempts++;
    if(peer_busy)return ESP_FAIL; /* Peer busy, file remains healthy. */
    assert(selected_valid);
    return sd_audio_catalog_ack(selected_session.session_id,selected_segment.index);
}
''')
replay = (root/'main/sd_audio_session_replay.inc').read_text(encoding='utf-8')
(out/'sd_file_fault_production_host.inc').write_text(replay[replay.index('static esp_err_t card_confirm_missing_locked('):replay.index('/* Signature retained')],encoding='utf-8')
(out/'sd_scheduler_host.inc').write_text(replay[replay.index('static esp_err_t card_replay_next_impl('):replay.rindex('#endif')], encoding='utf-8')
run('deferred_starvation', prefix + r'''
int main(int argc, char **argv) {
    const unsigned failures=argc>1 ? (unsigned)atoi(argv[1]) : 17U;
    const unsigned mode=argc>2 ? (unsigned)atoi(argv[2]) : 0U;
    const unsigned order=argc>3 ? (unsigned)atoi(argv[3]) : 0U;
    assert(sd_audio_catalog_init() == ESP_OK);
    for (unsigned i=0; i<failures+1; i++) {
        char id[48];
        if (i==failures) strcpy(id,"zz_good_last"); else snprintf(id,sizeof id,"old_%04u",i);
        sd_audio_session_t scene=create_session(id,mode,1,1);
        /* Directory name order is deliberately independent of boot-relative
         * time: ascending, descending, permuted, or entirely tied. */
        scene.ended_ms=1000+(order==0 ? i : order==1 ? failures-i : order==2 ? (i*37U)%(failures+1U) : 0);
        assert(sd_audio_catalog_save(&scene)==ESP_OK);
        make_index(id,1,false);
        sd_audio_segment_t part; assert(sd_audio_catalog_read(id,0,&part)==ESP_OK);
        /* Physical creation order is monotonic; boot clocks and directory
         * order cannot move the younger scene ahead of the retained head. */
        snprintf(part.path,sizeof(part.path),"./card_%s_%08u.ready",id,i+1U);
        assert(sd_audio_catalog_write(id,&part)==ESP_OK);
        FILE *f=fopen(part.path,"wb"); assert(f); fputs("local source retained",f); fclose(f);
    }
    nuwax_ws_client_t ws=0;
    for (unsigned pass=0; pass<4U; pass++) {
        sd_audio_catalog_replay_reset(); s_card_pass_error=ESP_OK;
        const unsigned before_attempts=old_attempts;
        test_now+=30000U;
        const unsigned max_ticks=((failures+16U)/16U+1U)*40U;
        for (unsigned tick=0; tick<max_ticks && old_attempts==before_attempts; tick++) {
            index_opens=manifest_opens=0;
            bool worked=false; const esp_err_t result=card_replay_next(&ws,1,false,&worked);
            assert(result==ESP_OK || result==ESP_ERR_TIMEOUT || result==ESP_FAIL);
            assert(index_opens<=16 && manifest_opens<=32 && !held);
        }
        assert(old_attempts==before_attempts+1U && !new_attempts);
        for (unsigned i=0;i<failures;i++) { char id[48]; snprintf(id,sizeof id,"old_%04u",i); sd_audio_segment_t part; assert(sd_audio_catalog_read(id,0,&part)==ESP_OK && !part.acknowledged && !part.missing); }
    }
    peer_busy=false;test_now+=30000U;sd_audio_catalog_replay_reset();s_card_pass_error=ESP_OK;
    for(unsigned tick=0;tick<(failures+1U)*64U && !new_attempts;tick++) {
        index_opens=manifest_opens=0;bool worked=false;
        const esp_err_t result=card_replay_next(&ws,1,false,&worked);
        assert(result==ESP_OK || result==ESP_ERR_TIMEOUT || result==ESP_FAIL);
        assert(index_opens<=16 && manifest_opens<=32 && !held);
    }
    assert(new_attempts==1U);
    for(unsigned i=0;i<failures;i++) {
        char id[48];snprintf(id,sizeof(id),"old_%04u",i);sd_audio_segment_t part;
        assert(sd_audio_catalog_read(id,0,&part)==ESP_OK && part.acknowledged && !part.missing);
    }
    printf("PASS: failures=%u mode=%u directory/time_order=%u; busy head retained; after release every older file ACKs before the younger scene.\n",failures,mode,order);
}
''')

