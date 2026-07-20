import { ComponentFixture, TestBed } from '@angular/core/testing';
import { HomeComponent } from './home.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { ReactiveFormsModule, FormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { Title } from '@angular/platform-browser';
import { provideRouter } from '@angular/router';
import { MessageModule } from 'primeng/message';
import { SelectModule } from 'primeng/select';
import { ChartModule } from 'primeng/chart';
import { ProgressBarModule } from 'primeng/progressbar';
import { TooltipModule } from 'primeng/tooltip';

import { HashSuffixPipe } from 'src/app/pipes/hash-suffix.pipe';
import { DiffSuffixPipe } from 'src/app/pipes/diff-suffix.pipe';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { AddressPipe } from 'src/app/pipes/address.pipe';
import { SatsPipe } from 'src/app/pipes/sats.pipe';
import { ByteSuffixPipe } from 'src/app/pipes/byte-suffix.pipe';

import { TooltipTextIconComponent } from 'src/app/components/tooltip-text-icon/tooltip-text-icon.component';
import { ConfettiComponent } from 'src/app/components/confetti/confetti.component';
import { SnowflakesComponent } from 'src/app/components/snowflakes/snowflakes.component';

import { SystemApiService } from 'src/app/services/system.service';
import { ThemeService } from 'src/app/services/theme.service';
import { QuicklinkService } from 'src/app/services/quicklink.service';
import { LoadingService } from 'src/app/services/loading.service';
import { ShareRejectionExplanationService } from 'src/app/services/share-rejection-explanation.service';
import { LocalStorageService } from 'src/app/local-storage.service';
import { DashboardEditService } from 'src/app/services/dashboard-edit.service';
import { LayoutService } from 'src/app/layout/service/app.layout.service';
import { SystemAsic as ISystemASIC, SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { of } from 'rxjs';

describe('HomeComponent', () => {
  let component: HomeComponent;
  let fixture: ComponentFixture<HomeComponent>;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [
        HomeComponent,
        TooltipTextIconComponent,
        ConfettiComponent,
        SnowflakesComponent
      ],
      imports: [
        ReactiveFormsModule,
        FormsModule,
        NoopAnimationsModule,
        MessageModule,
        SelectModule,
        ChartModule,
        ProgressBarModule,
        TooltipModule,
        HashSuffixPipe,
        DiffSuffixPipe,
        DateAgoPipe,
        AddressPipe,
        SatsPipe,
        ByteSuffixPipe
      ],
      providers: [
        provideRouter([]),
        provideHttpClient(),
        provideToastr(),
        SystemApiService,
        ThemeService,
        QuicklinkService,
        Title,
        LoadingService,
        ShareRejectionExplanationService,
        LocalStorageService,
        DashboardEditService,
        LayoutService
      ]
    });
    fixture = TestBed.createComponent(HomeComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('uses ASIC metadata for BZM warning and meter ranges', () => {
    component.configureAsicSettings({
      ASICModel: 'BZM',
      deviceModel: 'Bonanza',
      swarmColor: 'yellow',
      asicCount: 4,
      defaultFrequency: 50,
      frequencyOptions: [50],
      frequencyTunable: false,
      defaultVoltage: 2800,
      voltageOptions: [2800],
      voltageTunable: false,
    } as ISystemASIC);

    expect(component.minimumFrequency).toBe(50);
    expect(component.maxFrequency).toBe(50);
    expect(component.maxCoreVoltage).toBe(2.8);

    const info = {
      frequency: 50,
      version: 'test',
      axeOSVersion: 'test',
    } as ISystemInfo;
    component.handleSystemMessages(info, { duration: 0, startTime: null });
    expect(component.messages.some(message => message.type === 'FREQUENCY_LOW')).toBeFalse();

    info.frequency = 49;
    component.handleSystemMessages(info, { duration: 0, startTime: null });
    expect(component.messages.some(message => message.type === 'FREQUENCY_LOW')).toBeTrue();
  });

  it('renders the production mining, pool, topology, power, bridge, and result health', () => {
    component.activePoolURL = 'pool.example';
    component.activePoolPort = 3333;
    component.activePoolProtocol = 'SV1';
    component.info$ = of({
      version: 'mvo-test',
      currentWorkAgeSeconds: 4.2,
      poolConnectionInfo: 'Connected',
      sharesAccepted: 7,
      sharesRejected: 1,
      hashRate: 710,
      hashRate_1m: 700,
      asicHealth: {
        lifecycle: 'MINING',
        stateAgeSeconds: 125,
        asicCount: 4,
        expectedAsicCount: 4,
        activeEngineCount: 944,
        expectedEngineCount: 944,
        fixedFrequencyMHz: 800,
        fixedVoltageMV: 2800,
        measuredVoltageV: 2.8,
        boardTemperatureC: 64.5,
        fanPercent: 100,
        fanRPM: 5200,
        bridgeVersion: '0.0.1+mvo',
        bridgeProtocolMajor: 1,
        bridgeProtocolMinor: 2,
        bridgeCompatible: true,
        parserDiscardedBytes: 2,
        parserRecoveries: 1,
        addressMarkRealignments: 1,
        transportCrcFailures: 0,
        transportSequenceGaps: 0,
        transportDuplicateFrames: 0,
        transportDiscardedWireBytes: 0,
        bridgePioFifoOverflows: 0,
        bridgeSoftwareRingOverflows: 0,
        mappedResults: 240,
        locallyValidResults: 220,
        mappingRejections: 3,
        localRejections: 2,
        duplicateResults: 0,
        dispatchFailures: 0,
        lastFaultCode: 0,
        lastFault: '',
        automaticRetry: false,
        userActionRequired: false,
        recommendedAction: '',
      },
    } as ISystemInfo);

    fixture.detectChanges();

    const card = fixture.nativeElement.querySelector('[data-testid="asic-health"]') as HTMLElement;
    const text = card.textContent?.replace(/\s+/g, ' ') ?? '';
    expect(text).toContain('Miner health MINING');
    expect(text).toContain('pool.example:3333');
    expect(text).toContain('Work age: 4.2 s');
    expect(text).toContain('4 / 4 ASICs');
    expect(text).toContain('944 / 944 engines');
    expect(text).toContain('800 MHz at 2800 mV fixed');
    expect(text).toContain('2.800 V measured');
    expect(text).toContain('protocol 1.1');
    expect(text).toContain('Mapped results: 240');
    expect(text).toContain('Locally valid: 220');
    expect(text).not.toContain('validation stage');
    expect(text).not.toContain('operator lease');
    expect(text).not.toContain('manual arm');
  });

  describe('stale data and visibility state', () => {
    it('should set stale data error when visible and last message is old', () => {
      spyOnProperty(document, 'visibilityState', 'get').and.returnValue('visible');

      component['lastMessageTime'] = Date.now() - 10000;
      component.systemInfoError$.next({ duration: 0, startTime: null });

      component['checkStaleData']();

      expect(component.systemInfoError$.value.duration).toBe(10);
    });

    it('should NOT set stale data error when hidden and last message is old', () => {
      spyOnProperty(document, 'visibilityState', 'get').and.returnValue('hidden');

      component['lastMessageTime'] = Date.now() - 10000;
      component.systemInfoError$.next({ duration: 0, startTime: null });

      component['checkStaleData']();

      expect(component.systemInfoError$.value.duration).toBe(0);
    });

    it('should reset lastMessageTime and clear stale error when transitioning to visible', () => {
      spyOnProperty(document, 'visibilityState', 'get').and.returnValue('visible');

      const initialTime = Date.now() - 10000;
      component['lastMessageTime'] = initialTime;
      component.systemInfoError$.next({ duration: 10, startTime: initialTime });

      component.onVisibilityChange();

      expect(component.systemInfoError$.value.duration).toBe(0);
      expect(component.systemInfoError$.value.startTime).toBeNull();
      expect(component['lastMessageTime']).toBeGreaterThan(initialTime);
    });
  });
});
