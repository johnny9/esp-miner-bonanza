import { Component, OnDestroy, ViewChild } from '@angular/core';
import {
  Observable, Subscription, catchError, map, of, shareReplay,
  switchMap, takeWhile, timer
} from 'rxjs';
import { HttpErrorResponse, HttpEventType } from '@angular/common/http';
import { ToastrService } from 'ngx-toastr';
import { FileUploadHandlerEvent, FileUpload } from 'primeng/fileupload';
import { GithubUpdateService } from 'src/app/services/github-update.service';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LocalStorageService } from 'src/app/local-storage.service';
import { ModalComponent } from '../modal/modal.component';
import {
  BridgeInfo, BridgeUpdateStatus, SystemInfo
} from 'src/app/generated/models';

const IGNORE_RELEASE_CHECK_WARNING = 'IGNORE_RELEASE_CHECK_WARNING';

@Component({
    selector: 'app-update',
    templateUrl: './update.component.html',
    styleUrls: ['./update.component.scss'],
    standalone: false
})
export class UpdateComponent implements OnDestroy {

  public firmwareUpdateProgress: number = 0;
  public websiteUpdateProgress: number = 0;
  public bridgeUpdateProgress: number = 0;
  public bridgeUpdatePhase: string = '';

  public checkLatestRelease: boolean = false;
  public latestRelease$: Observable<any>;

  public info$: Observable<SystemInfo>;
  public bridgeInfo$: Observable<BridgeInfo | null>;

  @ViewChild('firmwareUpload') firmwareUpload!: FileUpload;
  @ViewChild('websiteUpload') websiteUpload!: FileUpload;
  @ViewChild('bridgeUpload') bridgeUpload?: FileUpload;

  @ViewChild('privacyModal') privacyModal?: ModalComponent;
  @ViewChild('progressModal') progressModal?: ModalComponent;

  public updateTarget: string = '';
  public updateStatus: 'progress' | 'success' | 'error' = 'progress';
  public updateMessage: string = '';
  private bridgePollSubscription?: Subscription;

  constructor(
    private systemService: SystemApiService,
    private liveDataService: LiveDataService,
    private toastrService: ToastrService,
    private loadingService: LoadingService,
    private githubUpdateService: GithubUpdateService,
    private localStorageService: LocalStorageService,
  ) {
    this.latestRelease$ = this.githubUpdateService.getReleases().pipe(map(releases => {
      return (releases as any)[0];
    }));

    this.info$ = this.liveDataService.info$;
    this.bridgeInfo$ = this.info$.pipe(
      switchMap(info => info.ASICModel === 'BZM'
        ? this.systemService.getBridgeInfo().pipe(catchError(() => of({
            available: false,
            versionQuerySupported: false,
            version: null,
            protocolMajor: null,
            protocolMinor: null,
          })))
        : of(null)),
      shareReplay({ bufferSize: 1, refCount: true }),
    );
  }

  ngOnDestroy(): void {
    this.bridgePollSubscription?.unsubscribe();
  }

  public get modalProgress(): number {
    if (this.updateTarget === 'AxeOS') return this.websiteUpdateProgress;
    if (this.updateTarget === 'Bridge') return this.bridgeUpdateProgress;
    return this.firmwareUpdateProgress;
  }

  otaUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.firmwareUpload.clear(); // clear the file upload component

    if (file.name != 'esp-miner.bin') {
      this.toastrService.error('Incorrect file, looking for esp-miner.bin.');
      return;
    }

    this.updateTarget = 'Firmware';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.performOTAUpdate(file)
      .subscribe({
        next: (event: any) => {
          if (event.type === HttpEventType.UploadProgress) {
            this.firmwareUpdateProgress = Math.round((event.loaded / (event.total as number)) * 100);
          } else if (event.type === HttpEventType.Response) {
            if (event.ok) {
              this.toastrService.success('Device restarted');
              this.updateStatus = 'success';
              this.updateMessage = 'Firmware updated. Device has been successfully restarted.';
            } else {
              this.updateStatus = 'error';
              this.updateMessage = event.statusText || 'An unknown error occurred.';
            }
          }
          else if (event instanceof HttpErrorResponse)
          {
            this.updateStatus = 'error';
            this.updateMessage = event.error?.message || event.error || event.message || 'Unknown error occurred';
          }
        },
        error: (err) => {
          this.updateStatus = 'error';
          this.updateMessage = err.error?.message || err.error || err.message || 'Unknown error occurred';
        },
        complete: () => {
          this.firmwareUpdateProgress = 0;
        }
      });
  }

  otaWWWUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.websiteUpload.clear(); // clear the file upload component

    if (file.name != 'www.bin') {
      this.toastrService.error('Incorrect file, looking for www.bin.');
      return;
    }

    this.updateTarget = 'AxeOS';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.performWWWOTAUpdate(file)
      .subscribe({
        next: (event: any) => {
          if (event.type === HttpEventType.UploadProgress) {
            this.websiteUpdateProgress = Math.round((event.loaded / (event.total as number)) * 100);
          } else if (event.type === HttpEventType.Response) {
            if (event.ok) {
              this.updateStatus = 'success';
              this.updateMessage = 'AxeOS updated. The page will reload in a few seconds.';
              setTimeout(() => {
                window.location.reload();
              }, 2000);
            } else {
              this.updateStatus = 'error';
              this.updateMessage = event.statusText || 'An unknown error occurred.';
            }
          }
          else if (event instanceof HttpErrorResponse)
          {
            this.updateStatus = 'error';
            this.updateMessage = event.error?.message || event.error || event.message || 'Unknown error occurred';
          }
        },
        error: (err) => {
          this.updateStatus = 'error';
          this.updateMessage = err.error?.message || err.error || err.message || 'Unknown error occurred';
        },
        complete: () => {
          this.websiteUpdateProgress = 0;
        }
      });
  }

  bridgeUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.bridgeUpload?.clear();

    if (!file.name.toLowerCase().endsWith('.bin')) {
      this.toastrService.error(
        'Incorrect file, select a raw RP2040 .bin image.');
      return;
    }

    this.updateTarget = 'Bridge';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    this.bridgeUpdateProgress = 0;
    this.bridgeUpdatePhase = 'uploading';
    if (this.progressModal) this.progressModal.isVisible = true;

    this.systemService.performBridgeUpdate(file).subscribe({
      next: event => {
        if (event.type === HttpEventType.UploadProgress) {
          this.bridgeUpdateProgress = event.total
            ? Math.round(event.loaded * 100 / event.total)
            : 0;
        } else if (event.type === HttpEventType.Response) {
          this.applyBridgeStatus(event.body);
          this.pollBridgeUpdate();
        }
      },
      error: err => {
        this.updateStatus = 'error';
        this.updateMessage = err.error?.message || err.error ||
          err.message || 'Bridge firmware upload failed';
      },
    });
  }

  private pollBridgeUpdate(): void {
    this.bridgePollSubscription?.unsubscribe();
    this.bridgePollSubscription = timer(0, 750).pipe(
      switchMap(() => this.systemService.getBridgeFirmwareUpdateStatus()),
      takeWhile(status => status.running, true),
    ).subscribe({
      next: status => this.applyBridgeStatus(status),
      error: err => {
        this.updateStatus = 'error';
        this.updateMessage = err.error?.message || err.error ||
          err.message || 'Unable to read bridge update status';
      },
    });
  }

  private applyBridgeStatus(status: BridgeUpdateStatus | null): void {
    if (!status) return;
    this.bridgeUpdatePhase = status.state;
    this.bridgeUpdateProgress = status.progress;
    if (status.state === 'complete') {
      this.updateStatus = 'success';
      this.updateMessage = status.currentVersion
        ? `Bridge firmware updated to ${status.currentVersion}. Restart ESP-Miner before mining.`
        : 'Bridge firmware updated successfully. Restart ESP-Miner before mining.';
      this.bridgeInfo$ = this.systemService.getBridgeInfo().pipe(
        catchError(() => of({
          available: true,
          versionQuerySupported: false,
          version: null,
          protocolMajor: null,
          protocolMinor: null,
        })),
      );
    } else if (status.state === 'failed') {
      this.updateStatus = 'error';
      this.updateMessage = status.error || 'Bridge firmware update failed';
    }
  }

  // https://gist.github.com/elfefe/ef08e583e276e7617cd316ba2382fc40
  public simpleMarkdownParser(markdown: string): string {
    const toHTML = markdown
      .replace(/^#{1,6}\s+(.+)$/gim, '<h4 class="mt-2">$1</h4>') // Headlines
      .replace(/\*\*(.+?)\*\*|__(.+?)__/gim, '<b>$1</b>') // Bold text
      .replace(/\*(.+?)\*|_(.+?)_/gim, '<i>$1</i>') // Italic text
      .replace(/\[(.*?)\]\((.*?)\s?(?:"(.*?)")?\)/gm, '<a href="$2" class="underline text-white" target="_blank">$1</a>') // Markdown links
      .replace(/(https?:\/\/github\.com\/.+\/(.+[^\s])+)/gim, (match, p1, p2) => `<a href="${p1}" target="_blank">${match.includes('/pull/') ? '#' : ''}${p2}</a>`) // Regular links
      .replace(/@([^\s]+)/gim, ' <a href="https://github.com/$1" target="_blank">@$1</a> ') // Username links
      .replace(/^\s*[-+*]\s?(.+)$/gim, '<li>$1</li>') // Unordered list
      .replace(/`([^`]+)`/gim, '<code class="bg-surface-100 border-round px-1">$1</code>') // Code
      .replace(/\r\n\r\n/gim, '<br>'); // Breaks

    return toHTML.trim();
  }

  public handleReleaseCheck(): void {
    if (this.localStorageService.getBool(IGNORE_RELEASE_CHECK_WARNING)) {
      this.checkLatestRelease = true;
    } else {
      if (this.privacyModal) {
        this.privacyModal.isVisible = true;
      }
    }
  }

  public continueReleaseCheck(skipWarning: boolean): void {
    this.checkLatestRelease = true;
    if (this.privacyModal) {
      this.privacyModal.isVisible = false;
    }

    if (!skipWarning) {
      return;
    }

    this.localStorageService.setBool(IGNORE_RELEASE_CHECK_WARNING, true);
  }
}
